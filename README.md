# tubane

**物理的な VR 機器を一切使わずに、PC 単体で SteamVR 環境を完全にエミュレートするための開発・デバッグ・VRSNS 用ツールです。**

SteamVR に仮想の HMD・Valve Index コントローラー（左右）・HTC Vive Tracker（初期値 3 台）を認識させ、
それらの位置・姿勢・ボタン入力を PC 上の 3D サンドボックス GUI からマウス / キーボードで自由に操作できるようにします。

> **現在の進捗: Step 1（ドライバのスケルトン + 共有メモリ定義）完了**
> サンドボックス GUI（Step 2）、映像出力ビューワー（Step 3）は未実装です。詳細は [ロードマップ](#ロードマップ) を参照してください。

---

## 目次

1. [構成概要](#構成概要)
2. [動作要件](#動作要件)
3. [ディレクトリ構成](#ディレクトリ構成)
4. [ビルド方法](#ビルド方法)
5. [SteamVR への登録](#steamvr-への登録)
6. [動作確認（Step 1）](#動作確認step-1)
7. [tubane_shm_probe の使い方](#tubane_shm_probe-の使い方)
8. [設定項目 (default.vrsettings)](#設定項目-defaultvrsettings)
9. [仮想デバイス一覧](#仮想デバイス一覧)
10. [座標系](#座標系)
11. [トラブルシューティング](#トラブルシューティング)
12. [ロードマップ](#ロードマップ)
13. [関連ドキュメント](#関連ドキュメント)

---

## 構成概要

```
┌──────────────────────── vrserver.exe (SteamVR) ────────────────────────┐
│  driver_tubane.dll  (tubane-driver)                                     │
│    ├─ HmdDevice          仮想 HMD  (IVRDisplayComponent → Step3: DirectMode)
│    ├─ ControllerDevice×2 Valve Index コントローラー (knuckles)          │
│    ├─ TrackerDevice×N    HTC Vive Tracker (腰・左足・右足 …)            │
│    └─ pose thread (~500Hz) ── 共有メモリから姿勢を読み SteamVR へ通知   │
└──────────────────────────────▲──────────────────────────────────────────┘
                               │  共有メモリ  Local\tubane_shared_state_v1
                               │  (SeqLock で保護された固定長構造体)
                               │  + Step3: D3D11 共有テクスチャ (NT ハンドル)
┌──────────────────────────────┴──────────────────────────────────────────┐
│  tubane-sandbox (Step 2)  C++ / D3D11 / Dear ImGui / ImGuizmo           │
│    3D の箱 + グリッド / フリーカメラ / デバイス選択 / ギズモ操作          │
│    HMD 映像ビューワー (左目 / 右目切替, FHD / WQHD)                      │
└─────────────────────────────────────────────────────────────────────────┘
```

| コンポーネント | 言語 / 技術 | 役割 |
|---|---|---|
| `tubane-driver` (`driver_tubane.dll`) | C++20, OpenVR Driver API | SteamVR にロードされ、仮想デバイスをスポーンする |
| `tubane-sandbox` | C++20, D3D11, Dear ImGui, ImGuizmo | 3D 空間上でデバイスを操作し、HMD 映像を表示する（Step 2 / 3） |
| `common` | ヘッダオンリー C++ | 共有メモリ構造体・SeqLock・初期配置を driver / sandbox で共有 |
| `tubane_shm_probe` | C++ コンソールツール | GUI 無しで共有メモリを読み書きする検証用ツール |

技術選定の理由は [docs/architecture.md](docs/architecture.md) にまとめています。

---

## 動作要件

| 項目 | 要件 |
|---|---|
| OS | Windows 10 / 11 (x64) |
| コンパイラ | Visual Studio 2022 または 2026（「C++ によるデスクトップ開発」ワークロード） |
| CMake | 3.24 以上（Visual Studio 同梱の CMake で可） |
| Git | FetchContent で依存ライブラリを取得するために必要 |
| SteamVR | Steam からインストール済みであること（実機 VR 機器は不要） |
| ネットワーク | 初回 configure 時のみ（OpenVR SDK を GitHub から取得） |

依存ライブラリはすべて CMake の `FetchContent` で自動取得されるため、手動でのダウンロードは不要です。

| ライブラリ | バージョン | 用途 |
|---|---|---|
| [OpenVR SDK](https://github.com/ValveSoftware/openvr) | v2.5.1（`TUBANE_OPENVR_VERSION` で変更可） | `openvr_driver.h`（ヘッダのみ使用。`openvr_api.lib` はリンクしない） |
| Dear ImGui / ImGuizmo | Step 2 で追加予定 | サンドボックス GUI |

---

## ディレクトリ構成

```
tubane/
├─ CMakeLists.txt              ルート。オプション: TUBANE_BUILD_DRIVER / TUBANE_BUILD_TOOLS
├─ CMakePresets.json           vs2026 / vs2022 / ninja プリセット
├─ cmake/
│   └─ Dependencies.cmake      FetchContent による依存取得 (OpenVR SDK)
├─ common/                     driver / sandbox 共通（ヘッダオンリー）
│   └─ include/tubane/
│       ├─ ipc_layout.h        共有メモリの構造体定義（プロトコルの唯一の定義元）
│       ├─ shm.h               CreateFileMapping ラッパー + SeqLock 読み書き
│       └─ default_state.h     各デバイスの初期配置（"ポン置き" 座標）
├─ driver/
│   ├─ CMakeLists.txt
│   ├─ src/
│   │   ├─ driver_main.cpp     HmdDriverFactory（DLL エクスポート）
│   │   ├─ server_provider.*   IServerTrackedDeviceProvider。デバイス生成・IPC・pose thread
│   │   ├─ device_base.*       全デバイス共通基底（姿勢変換・共通プロパティ）
│   │   ├─ hmd_device.*        仮想 HMD（IVRDisplayComponent）
│   │   ├─ controller_device.* Index コントローラー（入力コンポーネント）
│   │   ├─ tracker_device.*    Vive Tracker（ロール自動割り当て）
│   │   ├─ settings.*          driver_tubane 設定セクションの読み書き
│   │   └─ driver_log.h        vrserver.txt へのログ出力
│   └─ package/tubane/         SteamVR ドライバフォルダの雛形（ビルド時にコピーされる）
│       ├─ driver.vrdrivermanifest
│       └─ resources/settings/default.vrsettings
├─ tools/
│   ├─ shm_probe/              tubane_shm_probe（共有メモリ検証ツール）
│   ├─ register_driver.ps1     SteamVR へドライバ登録 (vrpathreg adddriver)
│   ├─ unregister_driver.ps1   登録解除
│   └─ steamvr_common.ps1      上記スクリプトの共通関数
├─ docs/
│   ├─ architecture.md         設計方針・技術選定の詳細
│   └─ ipc.md                  共有メモリプロトコル仕様
└─ sandbox/                    （Step 2 で追加）
```

---

## ビルド方法

### Visual Studio 2026 の場合

「x64 Native Tools Command Prompt」または PowerShell で実行します。
`cmake` に PATH が通っていない場合は、Visual Studio 同梱の
`C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
を使ってください。

```powershell
cd tubane
cmake --preset vs2026                  # configure（初回のみ OpenVR SDK を取得）
cmake --build --preset vs2026-release  # Release ビルド
# cmake --build --preset vs2026-debug  # Debug ビルド
```

Visual Studio 2022 の場合は `vs2022` / `vs2022-release` プリセットを使用します。
Visual Studio の IDE で「フォルダーを開く」から `tubane` を開いても、プリセットが自動で認識されます。

### ビルド成果物

| パス | 内容 |
|---|---|
| `build/vs2026/package/tubane/` | **SteamVR に登録するドライバフォルダ一式** |
| `build/vs2026/package/tubane/bin/win64/driver_tubane.dll` | ドライバ本体 |
| `build/vs2026/tools/shm_probe/<Config>/tubane_shm_probe.exe` | 共有メモリ検証ツール |

> **注意:** `driver_tubane.dll` は Debug / Release に関わらず同じ場所に出力されます（登録パスを固定するため）。
> 最後にビルドした構成の DLL が使われます。また、SteamVR 実行中は DLL がロックされるため、
> 再ビルド前に SteamVR を終了してください。

DLL は静的 CRT（`/MT`）でリンクしているため、依存は `KERNEL32.dll` のみです。
VC++ 再頒布可能パッケージの有無に影響されません。

---

## SteamVR への登録

SteamVR を終了した状態で実行します。

```powershell
# ドライバフォルダを SteamVR に登録（ビルドし直すだけで次回起動時に反映される）
powershell -ExecutionPolicy Bypass -File tools\register_driver.ps1
```

### 実機 HMD や他の仮想 HMD ドライバが入っている環境

Virtual Desktop・ALVR・実機 HMD のドライバなどが有効だと、SteamVR はそちらを HMD として選ぶ場合があります。
`-ForceDriver` を付けると `Steam\config\steamvr.vrsettings` に以下を書き込み、tubane の HMD を強制的に使うようにします
（書き換え前に `steamvr.vrsettings.tubane-backup-<日時>` としてバックアップを作成します）。

```jsonc
"steamvr": {
  "forcedDriver": "tubane",
  "activateMultipleDrivers": true
}
```

```powershell
powershell -ExecutionPolicy Bypass -File tools\register_driver.ps1 -ForceDriver
```

### 登録解除

```powershell
powershell -ExecutionPolicy Bypass -File tools\unregister_driver.ps1                      # 登録のみ解除
powershell -ExecutionPolicy Bypass -File tools\unregister_driver.ps1 -RemoveForcedDriver  # forcedDriver も削除
```

`-BuildDir build\vs2022` のように指定すると、別のビルドディレクトリを登録 / 解除できます。

---

## 動作確認（Step 1）

1. ビルドして `register_driver.ps1` で登録する
2. SteamVR を起動する
3. 次の点を確認する
   - SteamVR のステータスウィンドウに **HMD・コントローラー 2 本・トラッカー 3 台** が表示される
   - デスクトップの左上に、左右の目を横に並べた 1920×540 の SteamVR コンポジタウィンドウが開く（Step 1 の暫定表示。Step 3 で専用ビューワーに置き換え予定）
   - 「設定 → コントローラー → Vive トラッカーを管理」で、トラッカーが腰・左足・右足に割り当てられている
4. `<Steam>\logs\vrserver.txt` を `[tubane]` で検索し、次のようなログが出ていることを確認する

   ```
   [tubane] init: resolution=FHD trackers=3 freq=90.0
   [tubane] shared memory created (Ok)
   [tubane] tracker TUBANE-TRK-00 assigned default role TrackerRole_Waist
   [tubane] activated TUBANE-HMD-0001 (slot 0) as device 0
   ...
   ```

5. 別のターミナルで `tubane_shm_probe status` を実行し、driver block の `pid` と `heartbeat` が 0 でないこと、各デバイスに `vrId` が割り当てられていることを確認する
6. `tubane_shm_probe orbit` を実行すると、HMD が左右に首を振り、右手コントローラーが円を描きます。SteamVR のミラー表示やコンポジタウィンドウで動きを確認できます

---

## tubane_shm_probe の使い方

サンドボックス GUI が完成するまでの検証用ツールです。共有メモリの内容を表示したり、姿勢を直接書き込んだりできます。

```
tubane_shm_probe status                      driver / control ブロックを 1 回表示
tubane_shm_probe watch                       500ms ごとに表示（Ctrl+C で終了）
tubane_shm_probe orbit                       HMD の首振り + 右手の円運動（Ctrl+C で終了）
tubane_shm_probe set <slot> <x> <y> <z> [yawDeg]
                                             slot: 0=HMD 1=左手 2=右手 3以降=トラッカー
tubane_shm_probe reset                       全デバイスを初期配置に戻す
tubane_shm_probe resolution <FHD|WQHD>       解像度変更を要求（SteamVR 再起動で反映）
tubane_shm_probe trackers <0..8>             トラッカー台数変更を要求（SteamVR 再起動で反映）
tubane_shm_probe trigger <left|right> <0..1> トリガー値を設定（0.9 以上でクリック扱い）
```

例:

```powershell
# HMD を 1m 前に出して、右に 30 度向ける
tubane_shm_probe set 0 0 1.6 -1.0 -30
# 右手のトリガーを全押し
tubane_shm_probe trigger right 1
```

> **注意:** 書き込み系のコマンドは「サンドボックス」と同じ立場（control ブロックの唯一の書き込み手）として動作します。
> Step 2 以降、`tubane-sandbox` と同時に書き込み系コマンドを実行しないでください（単一ライター前提のプロトコルのため）。
> 読み取り専用の `status` / `watch` はいつでも実行できます。

---

## 設定項目 (default.vrsettings)

`driver/package/tubane/resources/settings/default.vrsettings` に既定値があります。
ユーザーごとの上書き値は `Steam\config\steamvr.vrsettings` の `driver_tubane` セクションに保存されます。

| キー | 型 | 既定値 | 説明 |
|---|---|---|---|
| `enable` | bool | `true` | ドライバの有効 / 無効 |
| `resolutionPreset` | string | `"FHD"` | 片目あたりのレンダーターゲット。`"FHD"`=1920×1080 / `"WQHD"`=2560×1440。**SteamVR 再起動で反映** |
| `trackerCount` | int | `3` | スポーンするトラッカーの台数（0〜8）。**SteamVR 再起動で反映** |
| `displayFrequency` | float | `90.0` | 仮想ディスプレイのリフレッシュレート（Hz） |
| `ipd` | float | `0.063` | 瞳孔間距離（m） |
| `windowX` / `windowY` | int | `0` / `0` | Step 1 の暫定コンポジタウィンドウの位置 |
| `windowWidth` / `windowHeight` | int | `1920` / `540` | 同ウィンドウのサイズ（左右の目を横並びで表示） |

`resolutionPreset` と `trackerCount` は、サンドボックス（または `tubane_shm_probe`）から共有メモリ経由で変更要求を出すと、
ドライバが `steamvr.vrsettings` に保存します。SteamVR は実行中のレンダーターゲットサイズ変更やデバイス数変更に追従しないため、
反映には SteamVR の再起動が必要です（ドライバは `restartRequired` フラグで再起動の必要性を通知します）。

---

## 仮想デバイス一覧

| スロット | シリアル | 種別 | エミュレート対象 | 初期座標 (m) |
|---|---|---|---|---|
| 0 | `TUBANE-HMD-0001` | HMD | tubane Virtual HMD | (0, 1.60, 0) |
| 1 | `TUBANE-KNU-L` | Controller | Valve Index コントローラー 左 | (-0.20, 1.10, -0.30) |
| 2 | `TUBANE-KNU-R` | Controller | Valve Index コントローラー 右 | (0.20, 1.10, -0.30) |
| 3 | `TUBANE-TRK-00` | GenericTracker | HTC Vive Tracker（腰） | (0, 0.95, 0.05) |
| 4 | `TUBANE-TRK-01` | GenericTracker | HTC Vive Tracker（左足） | (-0.12, 0.08, 0) |
| 5 | `TUBANE-TRK-02` | GenericTracker | HTC Vive Tracker（右足） | (0.12, 0.08, 0) |
| 6〜10 | `TUBANE-TRK-03`〜 | GenericTracker | 追加トラッカー（胸・肘・膝） | 床の後方に一列 |

- Index コントローラーは SteamVR 同梱の `{indexcontroller}` リソース（レンダーモデル `valve_controller_knu_1_0_left/right`、
  入力プロファイル `index_controller_profile.json`）を使用します
- Vive Tracker は `{htc}` リソース（`vr_tracker_vive_1_0`、`vive_tracker_profile.json`）を使用します
- トラッカーのロールは、初回起動時に `steamvr.vrsettings` の `trackers` セクションへ書き込みます。
  既に値がある場合は上書きしないため、SteamVR の UI から変更したロールは保持されます

### Index コントローラーの入力パス

| 種類 | パス |
|---|---|
| Boolean | `/input/system/click`, `/input/system/touch`, `/input/a/click`, `/input/a/touch`, `/input/b/click`, `/input/b/touch`, `/input/trigger/click`, `/input/trigger/touch`, `/input/grip/touch`, `/input/thumbstick/click`, `/input/thumbstick/touch`, `/input/trackpad/touch` |
| Scalar（0〜1） | `/input/trigger/value`, `/input/grip/value`, `/input/grip/force`, `/input/trackpad/force`, `/input/finger/index`, `/input/finger/middle`, `/input/finger/ring`, `/input/finger/pinky` |
| Scalar（-1〜1） | `/input/thumbstick/x`, `/input/thumbstick/y`, `/input/trackpad/x`, `/input/trackpad/y` |
| Haptic | `/output/haptic`（受信のみ。振動は無視） |
| Skeleton | 未実装（Step 2 / 3 で `CreateSkeletonComponent` を追加予定） |

---

## 座標系

tubane 全体（共有メモリ・サンドボックス・ドライバ）で **OpenVR の Standing 空間と同じ座標系** を使います。変換処理は一切挟みません。

- 右手系、**+X = 右、+Y = 上、-Z = 前方**
- 単位はメートル、角度はラジアン
- クォータニオンは **(w, x, y, z)** の順（`vr::HmdQuaternion_t` と同じ）
- 原点は床面上のプレイエリア中心

---

## トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| SteamVR に tubane のデバイスが出ない | `vrpathreg show` で登録を確認する。`vrserver.txt` に `[tubane]` のログが無ければ DLL がロードされていない（パス違い、または x86 ビルド） |
| 別の HMD（Virtual Desktop など）が使われる | `register_driver.ps1 -ForceDriver` で `forcedDriver` を設定する |
| `tubane_shm_probe status` が `NotFound` になる | ドライバが共有メモリを開いていない（SteamVR 未起動、またはドライバ未ロード） |
| `VersionMismatch` | driver と probe / sandbox のビルドが異なる `kProtocolVersion` を使っている。両方をビルドし直す |
| `MapFailed (win32 error 5)` | SteamVR を管理者権限で起動していると、権限の異なるプロセスから共有メモリにアクセスできない。SteamVR とツールを同じ権限で実行する |
| 再ビルド時に DLL を上書きできない | SteamVR (vrserver.exe) が DLL をロックしている。SteamVR を終了してから再ビルドする |
| 解像度 / トラッカー数の変更が反映されない | SteamVR を再起動する（実行中の変更は仕様上反映できない） |

---

## ロードマップ

- [x] **Step 1** — ドライバのスケルトン
  - [x] CMake + FetchContent によるビルド環境
  - [x] 共有メモリ構造体（`common/include/tubane/ipc_layout.h`）と SeqLock
  - [x] HMD / Index コントローラー×2 / Vive Tracker×N のスポーン
  - [x] 共有メモリからの姿勢反映（約 500Hz）とボタン / 軸入力
  - [x] 解像度 / トラッカー数の永続設定（`steamvr.vrsettings`）
  - [x] 検証ツール `tubane_shm_probe`、登録スクリプト
- [ ] **Step 2** — `tubane-sandbox`
  - [ ] D3D11 + Dear ImGui (docking) + ImGuizmo のアプリケーション骨格
  - [ ] ルームスケールの箱 / グリッド描画、フリーカメラ
  - [ ] デバイスの簡易モデル（軸表示）描画、クリックによる選択（レイキャスト）
  - [ ] ImGuizmo による移動 / 回転、数値入力パネル
  - [ ] 共有メモリへの姿勢書き込み、ドライバ状態表示
  - [ ] コントローラー入力パネル（ボタン、トリガー、スティック、指カール）
- [ ] **Step 3** — 映像出力とリアルタイム同期の仕上げ
  - [ ] `IVRDriverDirectModeComponent` の実装（スワップテクスチャセット）
  - [ ] 選択した目を D3D11 共有テクスチャ（名前付き NT ハンドル + Keyed Mutex）へコピー
  - [ ] サンドボックス側ビューワー（左目 / 右目の切替、FHD / WQHD）
  - [ ] GPU アダプタ LUID の照合
  - [ ] 骨格入力（Skeletal Input）

---

## 関連ドキュメント

- [docs/architecture.md](docs/architecture.md) — 設計方針、技術選定の理由、スレッドモデル、Step 3 の映像経路
- [docs/ipc.md](docs/ipc.md) — 共有メモリのレイアウト、SeqLock の手順、設定変更のハンドシェイク
