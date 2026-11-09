# tubane アーキテクチャ

このドキュメントでは、tubane の設計方針・技術選定の理由・各コンポーネントの内部構造を説明します。
共有メモリの詳細なバイナリ仕様は [ipc.md](ipc.md) を参照してください。

---

## 1. 目的と前提

- 物理 VR 機器なしで、SteamVR 上に **HMD 1 台・Valve Index コントローラー 2 本・HTC Vive Tracker N 台（既定 3 台）** を出現させる
- 各デバイスの姿勢と入力は、PC 上の 3D サンドボックス GUI（`tubane-sandbox`）から操作する
- HMD が描画した映像を、低遅延で専用ビューワーに表示する（左目 / 右目の切替、FHD / WQHD）
- 対象は **Windows x64 + SteamVR** に限定する。D3D11 共有テクスチャと Win32 共有メモリを前提にしている

---

## 2. 技術選定

### 2.1 ドライバ: C++20 + OpenVR Driver API

SteamVR のドライバは `vrserver.exe` にロードされる C++ の DLL でなければならないため、ここに選択の余地はありません。

- OpenVR SDK は **ヘッダのみ** を使用します。サーバードライバは `HmdDriverFactory` と
  `VR_INIT_SERVER_DRIVER_CONTEXT` で渡されるインターフェース経由でのみ vrserver とやり取りするため、
  `openvr_api.lib` のリンクは不要です
- CRT は静的リンク（`/MT`）にしています。vrserver のプロセス環境に VC++ 再頒布可能パッケージが無くても確実にロードできるようにするためです

### 2.2 サンドボックス: C++20 + D3D11 + Dear ImGui + ImGuizmo

| 候補 | 評価 |
|---|---|
| **C++ / D3D11 / Dear ImGui / ImGuizmo（採用）** | ドライバと同じ言語なので `common/` の構造体ヘッダをそのまま共有できる。`ID3D11Device1::OpenSharedResourceByName` で共有テクスチャを直接開ける。ImGuizmo に移動 / 回転ギズモが揃っている |
| C# / Unity | 3D 表示は容易だが、D3D11 共有テクスチャの受け取りにネイティブプラグインが必要。構造体定義を C# 側に二重管理することになる |
| Rust (wgpu など) | 共有テクスチャの受け取りに D3D11 / DXGI の interop 層が必要で、wgpu からは扱いづらい |
| Electron / Web | 共有テクスチャを扱えず、低遅延表示の要件を満たせない |

### 2.3 IPC: 共有メモリ + D3D11 共有テクスチャ

| データ | 方式 | 理由 |
|---|---|---|
| 姿勢・入力・設定 | Win32 共有メモリ（`CreateFileMapping`）+ SeqLock | 同一マシン内のプロセス間通信で最速。システムコール無しで読み書きでき、ロックも不要。固定長の構造体なのでシリアライズも不要 |
| HMD 映像 | D3D11 共有テクスチャ（名前付き NT ハンドル + Keyed Mutex） | GPU 内でコピーが完結し、CPU への読み戻しが発生しない |

検討して不採用にした方式:

- **Named Pipe / TCP / UDP / OSC** — 姿勢のように「最新値だけが意味を持つ」データにはキューが不要で、遅延とコピーが増えるだけです。
  プロセス間でイベントを確実に届ける必要（例: 「ボタンが 1 回押された」を取りこぼさない）が出てきた場合は、共有メモリ内のリングバッファで対応します
- **CPU 経由の画像転送** — WQHD × 90Hz を CPU に読み戻すと帯域とレイテンシが大きく、要件を満たせません

---

## 3. コンポーネント構成

```
common/include/tubane/
  ipc_layout.h     ── プロトコル定義（構造体・定数・enum）
  shm.h            ── SharedMemory クラス, SeqRead / SeqWrite
  default_state.h  ── 初期配置

driver/src/
  driver_main.cpp      HmdDriverFactory
  server_provider.*    ServerProvider : IServerTrackedDeviceProvider
  device_base.*        DeviceBase : ITrackedDeviceServerDriver
    ├─ hmd_device.*        HmdDevice        (+ IVRDisplayComponent)
    ├─ controller_device.* ControllerDevice (Index コントローラー)
    └─ tracker_device.*    TrackerDevice    (Vive Tracker)
  settings.*           driver_tubane 設定セクション
  driver_log.h         VRDriverLog ラッパー
```

### 3.1 ServerProvider

- `Init()`
  1. 設定 (`driver_tubane`) を読み込む
  2. 共有メモリを `CreateOrOpen` で開く（失敗しても致命的エラーにはせず、初期配置で動作を続ける）
  3. HMD → 左手 → 右手 → トラッカー×N の順に `TrackedDeviceAdded`
  4. トラッカーは追加前に `trackers` セクションへ既定ロールを書き込む（既に設定されていれば上書きしない）
  5. ドライバ状態ブロックを公開し、pose thread を起動する
- `RunFrame()`（vrserver のメインスレッド、フレーム毎）
  - `PollNextEvent` でイベントキューを空にする（ハプティクスは現状無視）
  - コントローラー入力を共有メモリから読み、**変化のあった値だけ** `UpdateBooleanComponent` / `UpdateScalarComponent` へ渡す
  - 設定リビジョンの変化を検出し、永続設定を保存する（[ipc.md の 5 章](ipc.md#5-設定変更のハンドシェイク)）
- `Cleanup()`
  - pose thread を停止・join してから、デバイスと共有メモリを解放する

### 3.2 スレッドモデル

| スレッド | 処理内容 | 周期 |
|---|---|---|
| vrserver メインスレッド | `Init` / `RunFrame` / `Cleanup` / `Activate` / 入力更新 / 設定反映 | SteamVR のフレーム毎 |
| tubane pose thread | 共有メモリから姿勢を読み、`TrackedDevicePoseUpdated` を呼ぶ。ドライバのハートビートを更新する | 約 500Hz（2ms） |

- 姿勢をフレーム周期（90Hz）ではなく 500Hz で送るのは、サンドボックス側の操作がコンポジタの予測に滑らかに反映されるようにするためです
- `DeviceBase::objectId_` は atomic で、`Activate` 前の `PushPose` は何もしません
- `GetPose()` は vrserver から任意のスレッドで呼ばれ得るため、最後の姿勢を mutex で保護しています

### 3.3 デバイスのエミュレーション

| デバイス | `Prop_ControllerType` | レンダーモデル | 入力プロファイル |
|---|---|---|---|
| HMD | — | `generic_hmd` | — |
| Index 左 / 右 | `knuckles` | `{indexcontroller}valve_controller_knu_1_0_left` / `_right` | `{indexcontroller}/input/index_controller_profile.json` |
| Vive Tracker | `vive_tracker` | `{htc}vr_tracker_vive_1_0` | `{htc}/input/vive_tracker_profile.json` |

`{indexcontroller}` と `{htc}` は SteamVR に同梱されているドライバのリソースです。他のドライバのリソースは
`{driver名}` プレフィックスで参照できるため、モデルやプロファイルを tubane 側で再配布する必要はありません。

### 3.4 HMD の表示経路

#### Step 1（現在）: Extended Mode

`IVRDisplayComponent` のみを実装し、`IsDisplayOnDesktop() == true` を返します。
SteamVR のコンポジタが `windowX/Y/Width/Height` の位置に通常のウィンドウを開き、左右の目を横に並べて描画します。
OpenVR SDK の `driver_sample`（null driver）と同じ方式です。レンズ歪みは恒等写像、FOV は水平 90° です。

#### Step 3（予定）: Direct Mode + 共有テクスチャ

```
アプリ ──Submit──▶ SteamVR コンポジタ
                       │ CreateSwapTextureSet で driver が確保したテクスチャに
                       │ 左右の目を描画（歪み補正前）
                       ▼
          IVRDriverDirectModeComponent::SubmitLayer / Present  (driver_tubane.dll)
                       │ viewerEye で選択した目を
                       │ CopyResource（GPU 内コピー）
                       ▼
          出力用テクスチャ  D3D11_RESOURCE_MISC_SHARED_NTHANDLE | SHARED_KEYEDMUTEX
                       │ IDXGIResource1::CreateSharedHandle(name = DriverBlock::frameTextureName)
                       ▼
          tubane-sandbox  ID3D11Device1::OpenSharedResourceByName
                       │ AcquireSync / ReleaseSync で同期
                       ▼
          ビューワーパネル（ImGui::Image）
```

- `IVRDriverDirectModeComponent` を選ぶのは、左右の目を **個別のテクスチャ** として受け取れるためです
  （`IVRVirtualDisplay` は合成・歪み補正後の 1 枚しか受け取れず、目の切替に余計な処理が必要になります）
- コンポジタとサンドボックスが **同じ GPU アダプタ** 上にある必要があるため、ドライバはアダプタの LUID を
  `DriverBlock::adapterLuid` に書き込み、サンドボックスは起動時に照合します
- 解像度（片目あたりのレンダーターゲット）は `GetRecommendedRenderTargetSize` で返します。
  SteamVR はセッション中のサイズ変更に追従しないため、FHD / WQHD の切替は SteamVR 再起動で反映されます。
  ビューワーウィンドウの拡大縮小は即時に反映できます

### 3.5 サンドボックス（tubane-sandbox）

```
sandbox/src/
  main.cpp          Win32 ウィンドウ + D3D11 + ImGui のメインループ（vsync 同期）
  app.*             App: パネル UI / ビューポート / ギズモ / ピッキング / 入力の合成
  renderer.*        Renderer: オフスクリーン 4x MSAA → resolve → ImGui::Image
  camera.*          Camera: フリーカメラ
  scene.*           デバイスの静的な情報（名前・色・ボックスモデル・ピック範囲）
  ipc_link.*        IpcLink: ControlBlock の単一ライター + DriverBlock の監視
```

#### 1 フレームの流れ

```
PollDriver()            DriverBlock を読み、ハートビートの変化で接続状態を判定（1 秒止まったら切断扱い）
SyncConfigFromDriver()  ドライバが新しく接続したら、実行中の解像度 / トラッカー数を UI に取り込む
ImGuizmo::BeginFrame()
パネル描画              Devices / Inspector / Controller Input / Driver / HMD Viewer
                        → IpcLink::Control()（姿勢・入力・設定）を直接編集する
DrawViewport()          カメラ操作 → ギズモ (Manipulate) → ピッキング → ラベル
                        → ComposeInput()（UI 状態 → ControllerInput）→ RenderScene()
Publish()               ControlBlock を SeqLock で共有メモリへ書き込み（ハートビート +1）
```

- **唯一の正となるデータはサンドボックスが持つ `ControlBlock` のコピー**です。UI はこれを直接編集し、毎フレーム丸ごと共有メモリへ書き込みます。
  起動時は共有メモリに残っている値（ドライバが作成した初期値や、前回のサンドボックスが残した姿勢）を読み込んでから始めます
- 3D シーンはオフスクリーンのテクスチャに描画し、Docking 可能な `Viewport` ウィンドウに `ImGui::Image` として表示します。
  ImGuizmo はそのウィンドウの描画リストと矩形に対して描画します
- 行列は DirectXMath の右手系関数（`XMMatrixLookToRH` / `XMMatrixPerspectiveFovRH`）で作り、OpenVR の座標系のまま描画します。
  DirectXMath の行列（行優先・行ベクトル）はメモリ配置が ImGuizmo の期待する形式と同じなので、変換せずに渡せます
- ピッキングは、マウス位置から作ったレイと各デバイスの OBB（ピック範囲を少し拡大したもの）の交差判定です
- HLSL は外部ファイルを持たず、起動時に `D3DCompile` でコンパイルします（exe 単体で動作させるため）

---

## 4. 座標系

すべてのコンポーネントで **OpenVR の Standing 空間** を使い、変換処理を挟みません。

- 右手系、+X = 右、+Y = 上、-Z = 前方、単位はメートル
- クォータニオンは (w, x, y, z)
- ドライバは `qWorldFromDriverRotation` / `qDriverFromHeadRotation` に単位クォータニオン、
  `vecWorldFromDriverTranslation` / `vecDriverFromHeadTranslation` にゼロを設定するため、共有メモリの値がそのままワールド座標になります

サンドボックス（D3D11）は左手系の行列ライブラリ（DirectXMath の `LH` 系関数）ではなく、
`XMMatrixPerspectiveFovRH` / `XMMatrixLookAtRH` などの右手系関数を使って、この座標系のまま描画します。

---

## 5. ビルドと配置

- CMake のルートでターゲットを登録し、依存ライブラリは `cmake/Dependencies.cmake` の `FetchContent` で取得します
- `driver_tubane` の出力先は `<build>/package/tubane/bin/win64/` に固定され、ポストビルドで
  `driver/package/tubane/`（マニフェストと既定設定）が同じフォルダにコピーされます
- `tools/register_driver.ps1` が `vrpathreg adddriver <build>/package/tubane` を実行するため、
  以後は再ビルドするだけで次回の SteamVR 起動時に反映されます

---

## 6. 既知の制約・今後の課題

| 項目 | 内容 |
|---|---|
| 骨格入力 | 未実装。指カールの値（`/input/finger/*`）だけを送っている。Step 2 / 3 で `CreateSkeletonComponent` と骨格の生成処理を追加する |
| ハプティクス | イベントは受信しているが無視している。サンドボックスに振動インジケーターとして表示する予定 |
| 速度・角速度 | 共有メモリに項目はあるが、サンドボックスは 0 を送っている。値を入れると SteamVR が姿勢を先読み予測するため、角速度の座標系（ワールド / ローカル）を検証してから Step 3 で有効にする |
| 権限 | 共有メモリは既定の DACL で作成される。SteamVR を管理者権限で起動すると、通常権限のサンドボックスからアクセスできない |
| 複数 GPU | Step 3 の共有テクスチャは同一アダプタ前提。ハイブリッド GPU のノート PC では、両プロセスを同じ GPU に割り当てる必要がある |
