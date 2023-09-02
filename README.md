# WofDump

指定ボリューム上のバッキング ソースを持つファイルを列挙します。

Usage:

    wofdump <volumename>

ボリューム名にはプレフィックス \\??\\ を付けてください。例えば ```\??\C:``` の様に指定します。   
ボリューム名を省略するとC:ドライブを選択します。

## Build

ソースからビルドする場合は、Windows7のWDKとSDKが必要です。詳細は下記を参照してください。

[Build方法](.\BUILD.md)

## License

Copyright (C) YAMASHITA Katsuhiro. All rights reserved.

Licensed under the [MIT](LICENSE) License.
