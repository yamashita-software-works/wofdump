//****************************************************************************
//*                                                                          *
//*  WofDump.cpp                                                             *
//*                                                                          *
//*  Create: 2023-07-28                                                      *
//*                                                                          *
//*  Author: YAMASHITA Katsuhiro                                             *
//*                                                                          *
//*  Copyright (C) YAMASHITA Katsuhiro. All rights reserved.                 *
//*  Licensed under the MIT License.                                         *
//*                                                                          *
//****************************************************************************
#include <ntifs.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <locale.h>
#include <winerror.h>
#include "ntnativeapi.h"
#include "ntwof.h"

PWSTR pszArgsVolumeName = NULL;
char fPrintHardLink = -1; // FALSE:No TRUE:Print HardLink
char fPrintHeader   = -1; // PRINTHDR_xxx
int nPrintAlgorithm = -1; // FILE_PROVIDER_COMPRESSION_xxx

#define PRINTHDR_NONE  0
#define PRINTHDR_FULL  1
#define PRINTHDR_HARDLINKMARK 2

typedef struct
{
	WOF_EXTERNAL_INFO *ExternalInfo;
	union
	{
		FILE_PROVIDER_EXTERNAL_INFO_V1 *FileInfo;
		WIM_PROVIDER_EXTERNAL_INFO     *WimInfo;
	};
} WOFINFODATA;

extern void PrintHelp();

CHAR *AlgorithmName[] =
{
	"XPRESS4K",
	"LZX",
	"XPRESS8K",
	"XPRESS16K"
};

VOID
PrintHeader(
	WOFINFODATA *pwof,
	BOOLEAN FirstLine
	)
{
	if( fPrintHeader == PRINTHDR_FULL )
	{
		CHAR *pch;
		if( FirstLine )
		{
			if( pwof->ExternalInfo->Provider == WOF_PROVIDER_FILE )
			{
				if( pwof->FileInfo->Algorithm < ARRAYSIZE(AlgorithmName) )
					pch = AlgorithmName[ pwof->FileInfo->Algorithm ];
				else
					pch = "Unk Algm";
			}
			else if( pwof->ExternalInfo->Provider == WOF_PROVIDER_WIM )
			{
				pch = "WIM";
			}
			else
			{
				pch = "Unknown";
			}
		}
		else
		{
			pch = ">";
		}
		printf("%10s  ",pch);
	}
	else if( fPrintHeader == PRINTHDR_HARDLINKMARK )
	{
		if( !FirstLine )
			printf("> ");
	}
}

VOID
PrintPath(
	UNICODE_STRING *pusPath,
	UNICODE_STRING *pusName
	)
{
	if( pszArgsVolumeName )
	{
		printf("%S",pszArgsVolumeName);
	}
	if( pusPath )
	{
		printf("%wZ",pusPath);
	}
	if( pusPath != NULL && pusName != NULL )
	{
		if( pusPath->Length >= 2 && pusPath->Buffer[ pusPath->Length/sizeof(WCHAR) ] != L'\\' )
			printf("\\");
	}
	if( pusName != NULL )
	{
		printf("%wZ",pusName);
	}
	printf("\n");
}

BOOLEAN
IsPrintAlgorithm(
	WOFINFODATA *pwof
	)
{
	if( nPrintAlgorithm == -1 )
		return TRUE;

	if( pwof->ExternalInfo->Provider == WOF_PROVIDER_FILE )
	{
		if( pwof->FileInfo->Algorithm == nPrintAlgorithm )
		{
			return TRUE;
		}
	}

	return FALSE;
}

NTSTATUS
NTAPI
OpenVolume(
	PHANDLE phVolume,
	PWSTR pszVolumeName
	)
{
	OBJECT_ATTRIBUTES ObjectAttributes;
	IO_STATUS_BLOCK IoStatus = {0};
	NTSTATUS Status;
	UNICODE_STRING usVolumeName;
	HANDLE hVolume = INVALID_HANDLE_VALUE;

	RtlInitUnicodeString(&usVolumeName,pszVolumeName);

	InitializeObjectAttributes(&ObjectAttributes,&usVolumeName,0,NULL,NULL);

	Status = NtOpenFile(&hVolume,
					GENERIC_READ|SYNCHRONIZE,
					&ObjectAttributes,
					&IoStatus,
					FILE_SHARE_READ|FILE_SHARE_WRITE,
					FILE_SYNCHRONOUS_IO_NONALERT|FILE_OPEN_FOR_BACKUP_INTENT|FILE_NON_DIRECTORY_FILE);

	if( Status == STATUS_SUCCESS )
		*phVolume = hVolume;
	else
		*phVolume = INVALID_HANDLE_VALUE;

	return Status;
}

NTSTATUS
NTAPI
OpenFileById(
	PHANDLE phFile,
	HANDLE hRoot,
	PFS_FILE_ID_DESCRIPTOR FileIdDesc,
	ULONG DesiredAccess,
	ULONG ShareAccess,
	ULONG OpenOptions
	)
{
	OBJECT_ATTRIBUTES ObjectAttributes;
	IO_STATUS_BLOCK IoStatus = {0};
	NTSTATUS Status;
	HANDLE hFile = INVALID_HANDLE_VALUE;
	UNICODE_STRING NtPathName;

	switch( FileIdDesc->Type )
	{
		case FsFileIdType:
			NtPathName.Length = NtPathName.MaximumLength = (USHORT)sizeof(FileIdDesc->FileId);
			NtPathName.Buffer = (PWCH)&FileIdDesc->FileId;
			break;
		case FsObjectIdType:
			NtPathName.Length = NtPathName.MaximumLength = (USHORT)sizeof(FileIdDesc->ObjectId);
			NtPathName.Buffer = (PWCH)&FileIdDesc->ObjectId;
			break;
		case FsExtendedFileIdType:
			NtPathName.Length = NtPathName.MaximumLength = (USHORT)sizeof(FileIdDesc->ExtendedFileId);
			NtPathName.Buffer = (PWCH)&FileIdDesc->ExtendedFileId;
			break;
		default:
			Status = STATUS_INVALID_PARAMETER;
			RtlSetLastWin32Error( RtlNtStatusToDosError(Status) );
			return Status;
	}

	InitializeObjectAttributes(&ObjectAttributes,&NtPathName,0,hRoot,NULL);

	OpenOptions |= FILE_OPEN_BY_FILE_ID;

	Status = NtOpenFile(&hFile,
					DesiredAccess,
					&ObjectAttributes,
					&IoStatus,
					ShareAccess,
					OpenOptions);

	if( Status == STATUS_SUCCESS )
		*phFile = hFile;
	else
		*phFile = INVALID_HANDLE_VALUE;

	RtlSetLastWin32Error( RtlNtStatusToDosError(Status) );

	return Status;
}

NTSTATUS
NTAPI
GetWofFileInformation(
	HANDLE FileHandle,
	WOFINFODATA *pwofData
	)
{
	NTSTATUS Status;
	IO_STATUS_BLOCK IoStatus = {0};

	pwofData->ExternalInfo = NULL;
	pwofData->FileInfo = NULL;

	ULONG cbBuffer = 256;
	UCHAR *pBuffer = (UCHAR *)malloc(cbBuffer);
	if( pBuffer == NULL )
	{
		return E_OUTOFMEMORY;
	}

	Status = NtFsControlFile(FileHandle,NULL,NULL,NULL,
					&IoStatus,
					FSCTL_GET_EXTERNAL_BACKING,
					NULL,0,
					pBuffer,cbBuffer);

	if( Status == STATUS_PENDING )
	{
		LARGE_INTEGER li;
		li.QuadPart = -(10000000 * 10); // maximum wait 10sec
		Status = NtWaitForSingleObject(FileHandle,FALSE,&li);
		if( Status != STATUS_SUCCESS )
		{
			free(pBuffer);
			return Status;
		}
		Status = IoStatus.Status;
	}

	if( Status == STATUS_SUCCESS && IoStatus.Information != 0 )
	{
		pBuffer = (UCHAR*)realloc(pBuffer,(ULONG)IoStatus.Information);

		WOF_EXTERNAL_INFO *pwei = (WOF_EXTERNAL_INFO *)pBuffer;

		pwofData->ExternalInfo = pwei;

		if( pwei->Provider == WOF_PROVIDER_FILE )
		{
			FILE_PROVIDER_EXTERNAL_INFO_V1 *pFile = (FILE_PROVIDER_EXTERNAL_INFO_V1 *)&pBuffer[8];
			// pFile->Version;
			// pFile->Algorithm
			// pFile->Flags
			pwofData->FileInfo = pFile;
		}
		else if( pwei->Provider == WOF_PROVIDER_WIM )
		{
			WIM_PROVIDER_EXTERNAL_INFO *pWIM = (WIM_PROVIDER_EXTERNAL_INFO *)&pBuffer[8];
			// pWIM->Version;
			// pWIM->Flags;
			// pWIM->DataSourceId;
			// pWIM->ResourceHash[WIM_PROVIDER_HASH_SIZE];
			pwofData->WimInfo = pWIM;
		}
		else
		{
			free(pwei);
			pwofData->ExternalInfo = NULL;
			Status = STATUS_UNSUCCESSFUL;
		}
	}
	else
	{
		free(pBuffer);
	}

	return Status;
}

#define FreeWofFileInformation(wof) free(wof.ExternalInfo)

typedef struct _FS_FILE_LINK_ENTRY_INFORMATION {
    ULONG NextEntryOffset;
    LONGLONG ParentFileId;
    ULONG FileNameLength;
    WCHAR FileName[1];
} FS_FILE_LINK_ENTRY_INFORMATION, *PFS_FILE_LINK_ENTRY_INFORMATION;

typedef struct _FS_FILE_LINKS_INFORMATION {
    ULONG BytesNeeded;
    ULONG EntriesReturned;
    FS_FILE_LINK_ENTRY_INFORMATION Entry;
} FS_FILE_LINKS_INFORMATION, *PFS_FILE_LINKS_INFORMATION;

NTSTATUS
NTAPI
GetHardlinkInformation(
	HANDLE hFile,
	PFS_FILE_LINKS_INFORMATION *LinkInformation
	)
{
	NTSTATUS Status;
	IO_STATUS_BLOCK IoStatus = {0};

	FILE_LINKS_INFORMATION *pLinks;
	ULONG cb = sizeof(FILE_LINKS_INFORMATION) + 4096;
	pLinks = (PFILE_LINKS_INFORMATION)malloc( cb );

	Status = NtQueryInformationFile(hFile,&IoStatus,pLinks,cb,FileHardLinkInformation);

	if( Status == STATUS_SUCCESS )
	{
		if( pLinks->BytesNeeded != cb )
			pLinks = (PFILE_LINKS_INFORMATION)realloc(pLinks,pLinks->BytesNeeded); // shrink
		*LinkInformation = (PFS_FILE_LINKS_INFORMATION)pLinks;
	}
	else
	{
		while( Status == STATUS_BUFFER_OVERFLOW )
		{
			cb =  pLinks->BytesNeeded;

			if( pLinks )
				free(pLinks);
			pLinks = (FILE_LINKS_INFORMATION*)malloc( cb );

			if( pLinks != NULL )
			{
				Status = NtQueryInformationFile(hFile,&IoStatus,pLinks,cb,FileHardLinkInformation);
				if( Status == STATUS_SUCCESS )
				{
					*LinkInformation = (PFS_FILE_LINKS_INFORMATION)pLinks;
				}
			}
			else
			{
				Status = STATUS_NO_MEMORY;
			}
		}
	}

	return Status;
}

VOID
DumpHardLinkFileNames(
	HANDLE VolumeHandle,
	HANDLE FileHandle,
	WOFINFODATA *pwof
	)
{
	NTSTATUS Status;
	FS_FILE_ID_DESCRIPTOR FileIdDesc;
	IO_STATUS_BLOCK IoStatus;

	FS_FILE_LINKS_INFORMATION *LinkInformation = NULL;
	Status = GetHardlinkInformation(FileHandle,&LinkInformation);

	FS_FILE_LINK_ENTRY_INFORMATION *LinkEntry;
	LinkEntry = &LinkInformation->Entry;

	for(ULONG i = 0; i < LinkInformation->EntriesReturned; i++ )
	{
		FileIdDesc.dwSize = sizeof(FileIdDesc);
		FileIdDesc.FileId.QuadPart = LinkEntry->ParentFileId;
		FileIdDesc.Type = FsFileIdType;

		OpenFileById(&FileHandle,VolumeHandle,&FileIdDesc,FILE_READ_ATTRIBUTES|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE,
					FILE_SYNCHRONOUS_IO_NONALERT|FILE_OPEN_FOR_BACKUP_INTENT|FILE_DIRECTORY_FILE);

		ULONG cbFileName = sizeof(FILE_NAME_INFORMATION) + 65536;
		FILE_NAME_INFORMATION *FileName = (FILE_NAME_INFORMATION *)malloc(cbFileName);

		if( FileName )
		{
			if( NtQueryInformationFile(FileHandle,
					&IoStatus,
					FileName,
					cbFileName,
					FileNameInformation
					) == STATUS_SUCCESS )
			{
				UNICODE_STRING usPath;
				usPath.Length = usPath.MaximumLength = (USHORT)FileName->FileNameLength;
				usPath.Buffer = FileName->FileName;
				UNICODE_STRING usFileName;
				usFileName.Length = usFileName.MaximumLength = (USHORT)(LinkEntry->FileNameLength * sizeof(WCHAR));
				usFileName.Buffer = LinkEntry->FileName;

				PrintHeader(pwof,(i == 0));
				PrintPath(&usPath,&usFileName);
			}

			free(FileName);
		}

		LinkEntry = (FS_FILE_LINK_ENTRY_INFORMATION *)((ULONG_PTR)LinkEntry + LinkEntry->NextEntryOffset);
	}
}

void Exit(PCWSTR psz,int code=-1)
{
	printf("%S\n",psz);
	exit(0);
}

NTSTATUS ParseArugment(int argc, WCHAR* argv[],PWSTR *ppszVolume)
{
	for(int i = 1; i < argc;)
	{
		if( argv[i][0] == L'/' || argv[i][0] == L'-' )
		{
			switch( argv[i][1] )
			{
				case L'h':
				case L'H':
					if( fPrintHeader != -1 )
						return STATUS_INVALID_PARAMETER;
					fPrintHeader = PRINTHDR_FULL;
					break;
				case L'k':
				case L'K':
					if( fPrintHeader != -1 )
						return STATUS_INVALID_PARAMETER;
					fPrintHeader = PRINTHDR_HARDLINKMARK; // Effective only when -L is specified.
					break;
				case L'l':
				case L'L':
					if( fPrintHardLink != -1 )
						return STATUS_INVALID_PARAMETER;
					fPrintHardLink = TRUE;
					break;
				case L'a':
				case L'A':
					if( nPrintAlgorithm != -1 )
						return STATUS_INVALID_PARAMETER;
					if( i + 1 < argc )
					{
						nPrintAlgorithm = _wtoi(argv[++i]);
						if( nPrintAlgorithm < 0 && nPrintAlgorithm >= FILE_PROVIDER_COMPRESSION_MAXIMUM )
							return STATUS_INVALID_PARAMETER;
					}
					break;
				default:
					return STATUS_INVALID_PARAMETER;
					break;
			}
		}
		else
		{
			if( *ppszVolume == NULL )
				*ppszVolume = argv[i];
			else
				return STATUS_INVALID_PARAMETER;
		}

		i++;
	}

	if( fPrintHeader == PRINTHDR_HARDLINKMARK && fPrintHardLink != TRUE )
		return STATUS_INVALID_PARAMETER;
	
	return 0;
}

//----------------------------------------------------------------------------
//
//  wmain()
//
//----------------------------------------------------------------------------
int __cdecl wmain(int argc, WCHAR* argv[])
{
	NTSTATUS Status;

	setlocale(LC_ALL,"");

	WCHAR szOpenVolumeName[64];

	if( argc > 1 )
	{
		if( wcscmp(argv[1],L"/?") == 0 || wcscmp(argv[1],L"-?") == 0 )
		{
			PrintHelp();
			return 0;
		}

		Status = ParseArugment(argc,argv,&pszArgsVolumeName);

		if( Status != STATUS_SUCCESS )
		{
			Exit(L"Invalid Parameter.");
		}
	}

	if( pszArgsVolumeName == NULL )
	{
		pszArgsVolumeName = L"C:";
	}

	SIZE_T cch = wcslen(pszArgsVolumeName);

	if( cch >= 2 && 
		((pszArgsVolumeName[0] >= L'A' && pszArgsVolumeName[1] <= L'Z')||
		 (pszArgsVolumeName[0] >= L'a' && pszArgsVolumeName[1] <= L'z')) &&
		pszArgsVolumeName[1] == L':' && pszArgsVolumeName[2] == L'\0')
	{
		wcscpy_s(szOpenVolumeName,ARRAYSIZE(szOpenVolumeName),L"\\??\\");
		wcscat_s(szOpenVolumeName,ARRAYSIZE(szOpenVolumeName),pszArgsVolumeName);
	}
	else if( (_wcsnicmp(pszArgsVolumeName,L"\\Device\\",8) == 0) ||
			 (_wcsnicmp(pszArgsVolumeName,L"\\??\\",4) == 0) )
	{
		wcscpy_s(szOpenVolumeName,ARRAYSIZE(szOpenVolumeName),pszArgsVolumeName);
	}
	else
	{
		wcscpy_s(szOpenVolumeName,ARRAYSIZE(szOpenVolumeName),L"\\??\\");
		wcscat_s(szOpenVolumeName,ARRAYSIZE(szOpenVolumeName),pszArgsVolumeName);
	}

	//
	// initial settings
	//
	if( fPrintHardLink == -1 )
		fPrintHardLink = FALSE;

	if( fPrintHeader == -1 )
		fPrintHeader = PRINTHDR_NONE;

	//
	// open target volume
	//
	HANDLE hVolume;
	Status = OpenVolume(&hVolume,szOpenVolumeName);
	if( Status != STATUS_SUCCESS )
	{
		switch( Status )
		{
			case STATUS_OBJECT_PATH_SYNTAX_BAD:
				printf("Path syntax bad.\n");
				break;
			case STATUS_OBJECT_NAME_NOT_FOUND:
				printf("Volume name not found.\n");
				break;
			case STATUS_ACCESS_DENIED:
				printf("Access denied.\n",Status);
				break;
			default:
				printf("Failed open the volume. 0x%08X\n",Status);
				break;
		}
		return 0;
	}

	//
	// enum backing files
	//
	WOF_EXTERNAL_FILE_ID id;
	IO_STATUS_BLOCK IoStatus;	
	WOFINFODATA wof;
	FS_FILE_ID_DESCRIPTOR FileIdDesc;

	for(;;)
	{
		RtlZeroMemory(&wof,sizeof(wof));

		RtlZeroMemory(&IoStatus,sizeof(IoStatus));
		Status = NtFsControlFile(hVolume,NULL,NULL,NULL,&IoStatus,FSCTL_ENUM_EXTERNAL_BACKING,0,0,&id,sizeof(WOF_EXTERNAL_FILE_ID));

		if( Status != STATUS_SUCCESS )
		{
			break;
		}

		HANDLE hFile;
		FileIdDesc.dwSize = sizeof(FileIdDesc);
		FileIdDesc.ExtendedFileId = id.FileId;
		FileIdDesc.Type = FsExtendedFileIdType;

		Status = OpenFileById(&hFile,hVolume,&FileIdDesc,FILE_READ_ATTRIBUTES|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE,
							FILE_SYNCHRONOUS_IO_NONALERT|FILE_OPEN_FOR_BACKUP_INTENT|FILE_NON_DIRECTORY_FILE);

		if( GetWofFileInformation(hFile,&wof) == STATUS_SUCCESS )
		{
			if( wof.ExternalInfo->Provider == WOF_PROVIDER_FILE && nPrintAlgorithm != -1 )
			{
				if( !IsPrintAlgorithm(&wof) )
				{
					FreeWofFileInformation(wof);
					NtClose(hFile);
					continue;
				}
			}
		}

		if( fPrintHardLink )
		{
			// Display all hard linked associated with the FildId.
			DumpHardLinkFileNames(hVolume,hFile,&wof);
		}
		else
		{
			// Display the FildId associated single path without hard link.
			PrintHeader(&wof,TRUE);

			ULONG cbFileName = sizeof(FILE_NAME_INFORMATION) + 65536;
			FILE_NAME_INFORMATION *FileName = (FILE_NAME_INFORMATION *)malloc(cbFileName);

			if( NtQueryInformationFile(hFile,&IoStatus,
						FileName,cbFileName,FileNameInformation) == STATUS_SUCCESS )
			{
				UNICODE_STRING usFileName;
				usFileName.Length = usFileName.MaximumLength = (USHORT)FileName->FileNameLength;
				usFileName.Buffer = FileName->FileName;
				PrintPath(&usFileName,NULL);
			}

			free(FileName);
		}

		FreeWofFileInformation(wof);

		NtClose(hFile);
	}

	switch( Status )
	{
		case STATUS_SUCCESS:
		case STATUS_NO_MORE_FILES:
			break;
		case STATUS_INVALID_DEVICE_REQUEST:
			printf("This volume is not support external backing source.\n");
			break;
		default:	
			printf("Error : 0x%08X\n",Status);
			break;
	}

	return 0;
}
