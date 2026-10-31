# Devotion — macOS Wireless DualSense to DualShock 4 Bridge
# 《還願 Devotion》macOS 無線 PS5 DualSense 轉 PS4 DualShock 4 控制器轉接層

> 🤖 **AI-Generated Project Notice / AI 全自動生成專案聲明**
>
> - **English**: **100% of the code, reverse engineering, low-level IOKit/Mono/Objective-C runtime interposition architecture, and documentation in this repository were generated entirely by AI (Google DeepMind Antigravity). Human involvement was strictly limited to in-game playtesting.**
> - **繁體中文**：**本專案中 100% 的程式碼、底層 IOKit / Mono / Objective-C Runtime 動態攔截架構、逆向分析與說明文件皆完全由 AI（Google DeepMind Antigravity）獨立撰寫生成；人類在開發過程中的參與僅限於實際進入遊戲進行遊玩測試（Playtesting）。**

---

## 📖 English Description

### Overview
The macOS version of **Devotion** (*Red Candle Games*) officially supports only a **wired Sony DualShock 4 (PS4)** controller and was built with Unity 2020.2.1f1 + InControl (compiled prior to the release of the PlayStation 5 DualSense controller).

When connecting a **Sony PlayStation 5 DualSense Wireless Controller** over Bluetooth on macOS:
1. Unity and InControl do not recognize `"DualSense Wireless Controller"` (`VID 0x054C`, `PID 0x0CE6`), falling back to an unmapped generic profile where only the Left Stick functions and all buttons/triggers/Right Stick are unresponsive.
2. On macOS Bluetooth, DualSense encapsulates its full 78-byte Report `0x31` payload (buttons, D-Pad hat switch, right stick, triggers, and hardware sequence counter) inside a 77-byte vendor-defined HID element (`UsagePage 0xFF00`, `Usage 0x3B`), causing standard `IOHIDDeviceGetValue` / `IOHIDValueGetIntegerValue` queries on individual button and axis elements to return `0`.

This project provides a **zero-modification, non-invasive dynamic library shim** (`libds4_shim.dylib`) injected via `DYLD_INSERT_LIBRARIES`. **No files inside `/Applications/Devotion.app` are modified.**

### Key Technical Features
- **IOKit Hardware Spoofing (`IOHIDDeviceGetProperty`)**: Spoofs the Bluetooth DualSense (`0x054C:0x0CE6`) as a wired USB Sony DualShock 4 controller (`VID 0x054C`, `PID 0x05C4`, Transport `"USB"`, Product `"Wireless Controller"`, Manufacturer `"Sony Interactive Entertainment"`).
- **Objective-C Runtime Swizzling (`GameController.framework`)**: Swizzles `GCController` `vendorName` and `productCategory` so macOS GameController reports `"Sony Interactive Entertainment Wireless Controller"`.
- **500Hz Bluetooth Report `0x31` (`0xFF00:0x3B`) Parser & Coalesced Queue Engine**:
  - Parses the 77-byte vendor HID report in real time and deduplicates packets via the DualSense 32-bit monotonic hardware packet counter.
  - Interposes `IOHIDDeviceGetValue`, `IOHIDValueGetIntegerValue`, and `IOHIDQueueCopyNextValueWithTimeout`.
  - Uses coalesced atomic dirty flags for all 6 analog axes (`LX`, `LY`, `RX`, `RY`, `L2`, `R2`) and drains pending raw queue events on every poll so the Right Stick camera control operates with **zero FIFO queue backlog and zero latency**.
- **Mono Internal Call Hooks (`UnityEngine.Input`)**: Hooks `GetJoystickNames`, `GetKeyInt`, `GetKeyDownInt`, `GetKeyUpInt`, `GetKeyString`, `GetKeyDownString`, `GetKeyUpString`, `GetButton`, `GetButtonDown`, `GetButtonUp`, `GetAxis`, and `GetAxisRaw` via `dlsym("mono_add_internal_call")` for complete compatibility across both Main Menu UI and in-game controls.

### Quick Start
1. Pair and connect your **PS5 DualSense Wireless Controller** to your Mac via Bluetooth.
2. Make sure `Devotion.app` is installed at `/Applications/Devotion.app`.
3. Double-click **`launch_devotion.command`** in Finder, or run in Terminal:
   ```bash
   ./launch_devotion.command
   ```
4. *(Optional)* To rebuild the universal (`x86_64` + `arm64`) dynamic library from source:
   ```bash
   make
   ```

### Control Mapping
| DualSense Control | In-Game Function (DualShock 4 Equivalent) |
| :--- | :--- |
| **Left Stick** | Move / Menu Navigation |
| **Right Stick** | Camera Look / Rotate Object |
| **Cross (✕)** | Confirm / Interact / Inspect |
| **Circle (◯)** | Cancel / Back / Put Away |
| **Square (□)** | Action / Use Held Item |
| **Triangle (△)** | Open Inventory / Notes |
| **D-Pad (Up / Down / Left / Right)** | Menu & Item Selection |
| **L1 / R1 / L2 / R2** | Bumpers & Triggers (Gaze / Focus) |
| **Options** | Pause Menu |

---

## 📖 繁體中文說明 (Traditional Chinese)

### 專案簡介
赤燭遊戲（Red Candle Games）出品的《**還願 Devotion**》macOS 版本原生僅支援**有線連接的 PS4 DualShock 4 控制器**（基於 Unity 2020.2.1f1 與舊版 InControl 輸入系統，於 PS5 DualSense 問世前編譯）。

當玩家在 macOS 上透過藍牙連接 **PS5 DualSense 無線控制器**時，會遇到兩個底層問題：
1. 遊戲與 InControl 無法識別 `"DualSense Wireless Controller"`（`VID 0x054C`、`PID 0x0CE6`），導致退回無設定檔的預設模式，除了左類比搖桿外，所有按鍵、板機與右類比搖桿皆無法運作。
2. 在 macOS 藍牙模式下，DualSense 會將完整的 78-byte `0x31` 藍牙輸入封包（包含所有按鍵、十字鍵、右搖桿、L2/R2 板機與硬體序號）封裝於 77-byte 的廠商自訂 HID 元件（`UsagePage 0xFF00`、`Usage 0x3B`）中，導致標準 IOKit `IOHIDDeviceGetValue` 與 `IOHIDValueGetIntegerValue` 讀取各按鍵與軸向時永遠回傳 `0`。

本專案透過 `DYLD_INSERT_LIBRARIES` 動態載入輕量級轉接層（`libds4_shim.dylib`），在記憶體中即時完成 DualSense 藍牙封包解碼與 DualShock 4 硬體偽裝，**完全不需要修改 `/Applications/Devotion.app` 內的任何遊戲檔案**。

### 核心技術特點
- **IOKit 硬體屬性偽裝 (`IOHIDDeviceGetProperty`)**：將藍牙 DualSense 偽裝為有線 USB 連接的 Sony DualShock 4 控制器（`VID 0x054C`、`PID 0x05C4`、Transport `"USB"`、名稱 `"Sony Interactive Entertainment Wireless Controller"`），自動啟用遊戲內建的 PS4 介面圖示與完整按鍵配置。
- **Objective-C Runtime 動態方法交換 (`GameController.framework`)**：攔截 `GCController` 的 `vendorName` 與 `productCategory`，確保 macOS 系統框架層同樣回報為 DualShock 4。
- **500Hz 藍牙封包解析與零延遲軸向合併引擎**：
  - 即時解析 `0xFF00:0x3B` 77-byte 藍牙封包，並透過 DualSense 硬體 32-bit 遞增封包序號進行去重與時序保護。
  - 針對左右搖桿與板機（`LX`、`LY`、`RX`、`RY`、`L2`、`R2`）採用 Atomic Coalesced Dirty Flags（最新狀態合併機制），並在每次輪詢時清空底層佇列積壓，徹底消除右搖桿視角轉動的輸入延遲與放開搖桿後的視角漂移。
- **Mono 內部呼叫攔截 (`UnityEngine.Input`)**：透過 `dlsym("mono_add_internal_call")` 完整攔截 Unity 輸入層，確保主選單與遊戲內所有互動操作 100% 順暢運作。

### 使用方式
1. 將您的 **PS5 DualSense 無線控制器**透過藍牙與 Mac 配對並連線。
2. 確認《還願》已安裝於 `/Applications/Devotion.app`。
3. 在 Finder 中雙擊執行 **`launch_devotion.command`**，或於終端機（Terminal）執行：
   ```bash
   ./launch_devotion.command
   ```
4. *(選用)* 若需從原始碼重新編譯通用架構（`x86_64` + `arm64`）動態庫：
   ```bash
   make
   ```
