<p align="center">
  <img src="assets/icon_mac.png" width="160" height="160" alt="DevotionRekindled Icon" />
</p>

<h1 align="center">DevotionRekindled</h1>

<p align="center">
  Wireless PS5 DualSense support and 120Hz ProMotion performance for <em>Devotion</em> on macOS.
</p>

<p align="center">
  <a href="https://github.com/passs0/DevotionRekindled/releases/latest"><img src="https://img.shields.io/github/v/release/passs0/DevotionRekindled?label=Latest%20Release&color=007AFF" alt="Latest Release" /></a>
  <img src="https://img.shields.io/badge/macOS-11.0%2B%20%7C%20Apple%20Silicon%20%26%20Intel-black?logo=apple" alt="macOS 11.0+" />
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License" /></a>
</p>

---

## What it does

- **Wireless DualSense support:** Play *Devotion* over Bluetooth with a PS5 DualSense controller — all buttons, triggers, D-pad, and right-stick camera movement work out of the box.
- **120Hz & smart Retina scaling:** Scales oversized Retina render buffers to `1920` wide (keeping your native aspect ratio) and unlocks 120 FPS without 30 FPS VSync drops.
- **Zero game-file changes:** Launches `/Applications/Devotion.app` with a runtime shim so your original game bundle stays untouched.
- **Optional overrides:** Set `DS4_TARGET_FPS` (default `120`) or `DS4_MAX_WIDTH` (default `1920`, or `0` for native Retina) if you want custom limits.

## Install

1. Make sure *Devotion* is installed at `/Applications/Devotion.app`.
2. Download **`DevotionRekindled-v1.1.4.dmg`** from [Releases](https://github.com/passs0/DevotionRekindled/releases/latest) and drag `DevotionRekindled.app` to `/Applications`.
3. Clear Gatekeeper on first launch, pair your DualSense over Bluetooth, and open **DevotionRekindled**:
```zsh
xattr -cr /Applications/DevotionRekindled.app
```

Or build from source:
```zsh
git clone https://github.com/passs0/DevotionRekindled.git && cd DevotionRekindled
make && make install
```

Built with [Google Antigravity](https://antigravity.google) — 100% AI-written code and assets, steered and playtested end-to-end by hand.

## License

[MIT](LICENSE)
