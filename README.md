# emu68kplus emulator (based on Musashi)

68008搭載のSBC(Single Board Computer) [emu68kplus](https://github.com/tendai22/emu68kplus.git)のシミュレータです。Linux/MacOS上で動作します。

680008エミュレータのベースとしたものが[Musashi](https://github.com/kstenerud/Musashi)です。MMUエミュレーションまで入っていてなんかいい感じです。

## 概要

emu68kplus SBCは、68008, 128k SRAM, PIC18F47Q43の3チップ構成のSBCです。PICがUARTのエミュレーションを行います。

本シミュレータでは、ROM領域なし、RAM領域128kB(00000 - 1FFFF)を持ちます。

UARTは、
|address|desripton|
|--|--|
|800A0|Data Register<br>Read: Received Data<br>Write: Transmitting Data|
|800A1|Control Register<br>b0: RxRDY(1:ready, 0:not ready)<br>b1: TxRDY(1:ready, 0:not ready)|
|||

SDカードサポートはありません。気が向いたら導入します。

## 使い方

引数にダンプファイルを指定して起動すると、ダンプ後実行を開始します。

```
% ./sim echo.X
```
この時の`echo.X`は以下の感じです。
```
=0 0000 1000
=4 0000 0080
=80 4eba 0008
=84 4eba 001c
=88 60f6
=8a 3f00
=8c 1039 0008 00a1
=92 0200 0002
=96 67f4
=98 301f
=9a 13c0 0008 00a0
=a0 4e75
=a2 1039 0008 00a1
=a8 0200 0001
=ac 67f4
=ae 1039 0008 00a0
=b4 4e75
!
```
`={address}`で書き込みアドレスを指定して、通常の16進文字列が書き込みデータを表します。最後の`!`はRAMダンプ命令で、これまでに書き込んだアドレス範囲でダンプします。

引数に複数のファイルを与えると順次読み込みます。すべて読み込んだのちに`run...`を出力した後プログラムの実行を開始(リセット解除、00000番地のベクトルを読み込みそこへ飛ぶ)します。

このプログラム(`echo.X`)はUARTから1バイト読み込み、読み込んだデータをUARTに書き出すもので、実行すると何も出力せずユーザ入力を待ちます。入力をエコーバック後、`ESC`を入力すると実行を停止しシェルプロンプトに戻ります。

<img width=500 src="img/exec_example.png">

> `ESC`をたたいたあと、シェルプロンプト出力後キーを叩いても何も表示されない場合、プログラム終了処理に失敗しています。エコーバックなしですが、`stty sane^CR`とたたいてみましょう。一度でだめなら二度三度たたくのです。これでエコーバックが復帰するはずです。

## ビルド方法

ディレクトリ`example`の下で`make`を実行するだけでよいです。

```
% cd example
% make
```

`example`の中にファイル`sim`が生成されます。これに引数を付けて実行するとよいです。

```
% ./sim echo.X
```
## BUGS

[Musashi github のREADME](https://github.com/kstenerud/Musashi)の最後には、「`m68kconf.h`を別の名前で用意して make のオプションでそのファイル名を指定してビルドしろ」と書いてありますが、まるっきり無視してオリジナルのまま使っています。まぁこのままでいいか。

フォルダ`example`の下の`sim.c`と`osd_linux.c`を変更しています。この辺、オリジナルとの差分や、emu68kplusという個別のハードウェア対応部分がわかりにくい。以下に「PORTING NOTE」で変更についてまとめたので、とりあえずこのままで行く。


## PORTING NOTE

emu68kplusというハードウェアをエミュレーションするための変更は、Musashi が呼び出すメモリアクセス関数を変更することになる。この変更では emu68kplus固有のハードウェアである UART を実現するため、UARTのレジスタの Read/Write の際に生じる挙動をメモリアクセス関数の下に組み込む。

ディレクトリ`example`の下の`sim.c`, `osd_linux.c`を書き換えている。

### I/Oポート(シリアルI/O)の実現

メモリアクセス関数から`input_device_read/write`を直接呼び出している。また、命令実行ループの中で、`update_user_input`, `output_device_update`, `input_device_update`を呼び出して状態更新している。

デバイスドライバの処理を、上位層(I/OポートをRead/Writeすることにより開始する処理)、下位層(データが外部から到着、出力可能になり外部に書き出すことにより開始する処理)の2種類に分けて考えるならば、`input_device_read`, `input_device_status`(新設), `output_device_write`は上位層の処理を行い、`update_user_input`, `output_device_update`, `input_device_update`は下位層の処理を行う。

#### 上位層

* `input_device_read`: UART_DREGからの読み出しを行う。外部からの到着データを取り出すことになる。
* `input_device_status`: UART_CREGからの読み出しを行う。送信レディ、受信レディ状態を読み出しバイトデータのbit0, bit1に反映させる。
* `output_device_write`: UART_DREGへの書き込みを行う。送信可能な場合は直ちに送信を開始し、送信可能フラグ(`g_output_device_ready`)をリセットする。送信が可能でない場合(あるデータを送信中であある場合)、書き出しデータを送信データに上書きし、今、送信中のデータが完了後に次のデータ送信を開始する。

#### 下位層

* `update_user_input`: 標準入力(ファイルディスクリプタ0番)の入力データの有無を判断し、あればその1バイトを読み出し後、'~', 'ESC'の解釈・処理を行う。読み込んだデータは最終的に`g_input_device_value`に書き込まれ、フラグ`g_input_device_ready`に1がセットされる。
* `input_device_update`: データがレディの場合、割り込みINT_INPUT_DEVICEをセットする。
* `output_device_update`: 書き込みデータが`g_output_device_data`に格納されている場合に標準出力にその1バイトを書き込み、割り込みINT_OUTPUT_DEVICEをクリアする。出力完了待ちカウントを開始する。

#### UARTデバイスの状態

以下のグローバル変数と、`irq_controller`の`IRQ_INPUT_DEVICE`, `IRQ_OUTPUT_DEVICE`の状態が
それである。UARTの「データレジスタ」「シフトレジスタ」「送信完了フラグ」「データレジスタ書き込み可能フラグ」「受信データありフラグ」に該当する。

```
int     g_input_device_value = -1;
int     g_input_device_ready = 0;         /* Current status in input device */

unsigned int g_output_device_ready = 0;         /* 1 if output device is ready */
time_t  g_output_device_last_output;       /* Time of last char output */
```

#### 下位デバイスの扱い

下位デバイスは、Linuxのttyドライバである。ファイルディスクリプタを介しての read/writeを`fgetc`, `printf`呼び出し経由で利用している。

<stdio.h>ファイルI/Oのバッファリングを止める(`fgetc`/`printf`がバッファリングなしでread/writeシステムコールを呼び出す)ように、`setbuf`でNULL値をセットしている。

Linuxのttyドライバは、通常は「行編集モード(COOKEDモード)」であり、1文字キーをたたいても即座にアプリに1バイトが渡らない。行編集モードを解除し1文字押すごとにアプリに1文字わたるモード(RAWモード)に切り替えている。ttyドライバのモード切替は`changemode`関数で行う。引数に1を渡せばRAWモードにする。具体的には`ICANON`と`ECHO`ビットだけを落としている。引数に0を渡せばCOOKEDに戻す。

割り込みのキャッチは行っていない。SIGINT, SIGTERM, SIGQUITにより`sim`コマンドを停止させると`changemode(0);`を実行しそこなうのでttyがRAWモードのままになってしまう。シグナルハンドラを整えてこのあたりもちゃんとさせたいところである。

キー入力の有無を判断するために、`kbhit`関数が用意されている。ファイルディスクリプタ0に対する`select`呼び出しで結果を返している。今回の変更でtimeout指定に1msを指定して、連続した`kbhit`呼び出しでもビジーループにならないようにしておいた。1ループで68000の1命令実行なので、1msはちょっと大きすぎるかもしれない。

`kbhit`関数と`changemode`関数は`osd_linux.c`で定義されている。

### 割り込み機構

割り込み機構について特に追加の実装を行っていない。もともとのエミュレータそのままである。ただし、シリアルI/Oでの割り込み発生・リセットは元の実装に入っており、割り込みハンドラを有効にすることでそのまま動く可能性は高い。

本エミュレータではもともとの`sim.c`に実装されていた割り込み処理機能をそのまま残しているが、誰も割り込み要因を発生させないので割り込み機能は存在しないのと同じである。emu68kplusではUARTからの割り込みは今のところ実現していない。PICのファームウェアを書き換えれば割り込みも実現できるので、必要になればその時に行う。エミュレータの割り込み機能もその時に火を噴くだろう。