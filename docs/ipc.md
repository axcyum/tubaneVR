# tubane IPC プロトコル仕様

`tubane-driver`（vrserver.exe 内）と `tubane-sandbox` / ツール類の間でやり取りする共有メモリの仕様です。
定義の本体は [`common/include/tubane/ipc_layout.h`](../common/include/tubane/ipc_layout.h) で、本書はその解説です。
**両者が食い違った場合はヘッダの定義が正です。**

---

## 1. 概要

| 項目 | 値 |
|---|---|
| オブジェクト名 | `Local\tubane_shared_state_v1`（ログオンセッション単位の名前空間） |
| サイズ | `sizeof(tubane::ipc::SharedState)`（固定長） |
| 作成 | `CreateFileMappingW(INVALID_HANDLE_VALUE, …, PAGE_READWRITE, …)`（ページファイルを裏付けにした匿名マッピング） |
| マジック | `0x454E4254`（"TBNE"） |
| プロトコルバージョン | `kProtocolVersion = 1` |
| バイトオーダー | リトルエンディアン（x64 のみ対象） |
| 生存期間 | いずれかのプロセスがハンドルを開いている間だけ存在する。全員が閉じると消える |

---

## 2. 初期化手順

driver と sandbox のどちらが先に起動しても動作するように、**最初にマッピングを作成したプロセスが初期化を担当** します。

```
CreateFileMappingW(...)
  ├─ GetLastError() != ERROR_ALREADY_EXISTS  →  作成者
  │     version = kProtocolVersion
  │     size    = sizeof(SharedState)
  │     control ← FillDefaultControl()   (default_state.h の初期配置)
  │     driver  ← FillDefaultDriver()
  │     magic.store(kMagic, release)      ← 最後に公開
  │
  └─ ERROR_ALREADY_EXISTS                    →  参加者
        magic == kMagic になるまで待つ（タイムアウト 2 秒 → InitTimeout）
        version / size を照合（不一致 → VersionMismatch）
```

OS が新規ページをゼロで埋めて渡すため、`magic` が 0 の間は「初期化中」と判断できます。

---

## 3. メモリレイアウト

```
SharedState
├─ magic            atomic<uint32>   kMagic（初期化完了後）
├─ version          uint32           kProtocolVersion
├─ size             uint32           sizeof(SharedState)
├─ reserved[13]
├─ controlLock      SeqLock          (64 byte, キャッシュライン境界)
├─ control          ControlBlock     ← sandbox が書き、driver が読む
├─ driverLock       SeqLock          (64 byte)
└─ driver           DriverBlock      ← driver が書き、sandbox が読む
```

### 3.1 ControlBlock（sandbox → driver）

| フィールド | 型 | 説明 |
|---|---|---|
| `sandboxHeartbeat` | uint64 | sandbox がフレーム毎にインクリメントする。0 は「sandbox 未接続」 |
| `sandboxProcessId` | uint32 | sandbox の PID |
| `config` | ControlConfig | 設定（下記） |
| `poses[kMaxDevices]` | DevicePose[11] | スロット毎の姿勢 |
| `controllers[2]` | ControllerInput[2] | [0] = 左手、[1] = 右手 |

#### ControlConfig

| フィールド | 型 | 種別 | 説明 |
|---|---|---|---|
| `revision` | uint32 | — | sandbox がユーザー操作で設定を変えるたびにインクリメントする |
| `viewerEye` | ViewerEye | 即時反映 | 共有テクスチャへコピーする目（`Left`=0 / `Right`=1）。Step 3 で使用 |
| `resolution` | ResolutionPreset | 再起動で反映 | `FHD`=0（1920×1080）/ `WQHD`=1（2560×1440）、片目あたり |
| `trackerCount` | uint32 | 再起動で反映 | スポーンするトラッカーの台数（0〜`kMaxTrackers`） |

### 3.2 DevicePose

| フィールド | 型 | 説明 |
|---|---|---|
| `position[3]` | double | メートル。OpenVR Standing 空間（+X 右、+Y 上、-Z 前方） |
| `rotation[4]` | double | 単位クォータニオン **(w, x, y, z)** |
| `velocity[3]` | double | m/s（ワールド座標） |
| `angularVelocity[3]` | double | rad/s（ワールド座標） |
| `flags` | uint32 | `kPoseValid`(1) = 姿勢が有効 / `kPoseConnected`(2) = 接続中として報告 |

- `kPoseValid` が無い場合、driver は `TrackingResult_Running_OutOfRange` / `poseIsValid=false` で報告します
- 長さがほぼ 0 のクォータニオンは、driver 側で単位クォータニオンに置き換えます（NaN の発生を防ぐため）

### 3.3 スロット割り当て

| スロット | デバイス |
|---|---|
| 0 (`kSlotHmd`) | HMD |
| 1 (`kSlotLeftHand`) | 左手 Index コントローラー |
| 2 (`kSlotRightHand`) | 右手 Index コントローラー |
| 3〜10 (`TrackerSlot(i)`) | トラッカー i（0=腰、1=左足、2=右足、3=胸、4=左肘、5=右肘、6=左膝、7=右膝） |

### 3.4 ControllerInput

| フィールド | 型 | 範囲 | 対応する入力パス |
|---|---|---|---|
| `buttons` | uint32 | ビットマスク | 下表 |
| `trigger` | float | 0〜1 | `/input/trigger/value` |
| `gripValue` | float | 0〜1 | `/input/grip/value` |
| `gripForce` | float | 0〜1 | `/input/grip/force` |
| `thumbstickX` / `Y` | float | -1〜1 | `/input/thumbstick/x` / `y` |
| `trackpadX` / `Y` | float | -1〜1 | `/input/trackpad/x` / `y` |
| `trackpadForce` | float | 0〜1 | `/input/trackpad/force` |
| `finger[4]` | float | 0〜1 | `/input/finger/index` / `middle` / `ring` / `pinky` |

| ビット | 定数 | 入力パス |
|---|---|---|
| 0 | `kBtnSystemClick` | `/input/system/click` |
| 1 | `kBtnSystemTouch` | `/input/system/touch` |
| 2 | `kBtnAClick` | `/input/a/click` |
| 3 | `kBtnATouch` | `/input/a/touch` |
| 4 | `kBtnBClick` | `/input/b/click` |
| 5 | `kBtnBTouch` | `/input/b/touch` |
| 6 | `kBtnTriggerClick` | `/input/trigger/click` |
| 7 | `kBtnTriggerTouch` | `/input/trigger/touch` |
| 8 | `kBtnGripTouch` | `/input/grip/touch` |
| 9 | `kBtnThumbstickClick` | `/input/thumbstick/click` |
| 10 | `kBtnThumbstickTouch` | `/input/thumbstick/touch` |
| 11 | `kBtnTrackpadTouch` | `/input/trackpad/touch` |

### 3.5 DriverBlock（driver → sandbox）

| フィールド | 型 | 説明 |
|---|---|---|
| `driverHeartbeat` | uint64 | pose thread が約 500Hz でインクリメントする。増えなくなったら driver 停止とみなせる |
| `driverProcessId` | uint32 | vrserver.exe の PID |
| `activeTrackerCount` | uint32 | 今回のセッションで実際にスポーンしたトラッカーの数 |
| `renderWidth` / `renderHeight` | uint32 | 今回のセッションで使用中の片目あたりのレンダーターゲット |
| `activeResolution` | ResolutionPreset | 今回のセッションで有効なプリセット |
| `restartRequired` | uint32 | 1 = 保存済み設定が実行中のセッションと異なる（SteamVR の再起動が必要） |
| `appliedRevision` | uint32 | driver が最後に処理した `ControlConfig::revision` |
| `adapterLuid` | uint64 | コンポジタの GPU アダプタ LUID（Step 3） |
| `frameIndex` | uint64 | 共有テクスチャへ書き込んだフレーム数（Step 3） |
| `frameTextureName[64]` | wchar_t | 共有テクスチャの NT ハンドル名（Step 3） |
| `deviceIds[kMaxDevices]` | uint32 | スロット毎の OpenVR デバイスインデックス。`0xFFFFFFFF` は未割り当て |

---

## 4. SeqLock

各ブロックは **書き込み手が 1 つだけ** であることを前提に、SeqLock で保護します。
ロックを取らずに読み取りでき、書き込み側が読み取り側を待つこともありません。

### 書き込み（`SeqWrite`）

```
s = seq.load(relaxed)
seq.store(s + 1, relaxed)        // 奇数 = 書き込み中
fence(release)
<ブロックを書き換える>
seq.store(s + 2, release)        // 偶数 = 一貫した状態
```

### 読み取り（`SeqRead`）

```
loop (最大 256 回):
    s1 = seq.load(acquire)
    if s1 が奇数: YieldProcessor(); continue
    memcpy(out, block)
    fence(acquire)
    s2 = seq.load(relaxed)
    if s1 == s2: return true     // 書き込みと重ならずにコピーできた
return false                     // 書き込み手が停止中・異常終了した可能性
```

### 書き込み手の異常終了

書き込み中にプロセスが落ちると、カウンタが奇数のまま残り、読み取りが常に失敗します。
書き込み手は接続時に `SeqRecoverWriter()` を呼び、カウンタが奇数なら偶数に戻します
（driver は `Init()` で `driverLock` に対して実行します。sandbox は Step 2 で `controlLock` に対して実行します）。

読み取りに失敗した場合、driver の pose thread は **直前に読めた値を使い続けます**（1 回の失敗で初期配置に戻ったりしないようにするため）。

---

## 5. 設定変更のハンドシェイク

解像度とトラッカー台数は SteamVR の再起動後にしか反映できないため、`steamvr.vrsettings` に永続化します。
共有メモリはプロセスがすべて終了すると消えるので、永続設定の保存先には使えません。

```
sandbox                                   driver
───────                                   ──────
起動時: DriverBlock から activeResolution /
        activeTrackerCount を読み、UI に表示

ユーザーが WQHD を選ぶ
config.resolution = WQHD
config.revision++            ──────▶      RunFrame: revision の変化を検出
                                          steamvr.vrsettings に resolutionPreset="WQHD" を保存
                                          restartRequired = (WQHD != 実行中のプリセット)
                             ◀──────      DriverBlock.restartRequired = 1
                                          DriverBlock.appliedRevision = revision
「SteamVR を再起動してください」と表示
```

- driver は `Init()` の時点の `revision` を記録し、**それ以降に revision が変化したときだけ** 設定を保存します。
  そのため、共有メモリに残っている古い値や初期値が、ユーザーの保存済み設定を上書きすることはありません
- `viewerEye` は即時に反映される値なので、revision とは関係なく毎フレーム参照されます（Step 3）

---

## 6. バージョン管理のルール

- `ipc_layout.h` の **バイナリレイアウトを変更したら、必ず `kProtocolVersion` を上げる**
  （フィールドの追加・削除・並べ替え・型変更、`kMaxDevices` などの定数変更を含む）
- 互換性を保ったまま拡張したい場合は、既存の `reserved` フィールドを使う（この場合はバージョンを上げなくてよい）
- 共有メモリ名にもバージョン（`_v1`）が含まれています。大きな非互換変更の際は名前も変えると、
  古いプロセスが残っていても衝突しません

---

## 7. Step 3 で追加する映像経路（予定）

| 項目 | 内容 |
|---|---|
| テクスチャ | `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`（コンポジタのスワップテクスチャ形式に合わせる）、片目分のサイズ |
| 共有方法 | `D3D11_RESOURCE_MISC_SHARED_NTHANDLE \| D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`、`IDXGIResource1::CreateSharedHandle` で名前付き NT ハンドルを作成 |
| 名前 | `DriverBlock::frameTextureName`（解像度変更などで作り直すときは名前を変え、sandbox は開き直す） |
| 同期 | Keyed Mutex。driver が key 0 で Acquire → コピー → key 1 で Release、sandbox が key 1 で Acquire → 描画 → key 0 で Release |
| フレーム通知 | `DriverBlock::frameIndex` をインクリメント。sandbox は値が変わったときだけ描画を更新する |
| GPU 照合 | `DriverBlock::adapterLuid` と sandbox が使うアダプタの LUID を比較し、異なる場合は警告を表示する |
