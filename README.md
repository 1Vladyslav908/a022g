# Dungeon Master Kernel
Dungeon Master Kernel (DMK) is a custom ARM32 kernel for Samsung Galaxy A02 (SM-A022G).
Based on the Physwizz A02 kernel source. Original copyright and contributor notices are preserved.
Important note: This kernel is rough/experimental and won't give you a noticeable performance boost. I mostly build it for myself, but I'm sharing it anyway — feel free to grab it, tinker with it, and modify it. Source code link below. Based on a modified stock kernel by physwizz.

Dungeon Master Kernel (DMK)
# Samsung Galaxy A02 (MT6739) · Linux 4.14.186

Differences from stock:

    Scheduler: Adjusted CFS latency and min granularity.
    Memory: Simple LMK (lock/UAF fixes), tuned MINFREE and swappiness.
    GPU: Adjusted GED DVFS margin.
    Touch Boost: Tweaked input event handling and touch boost.
    I/O Scheduler: Removed CFQ and Deadline, added Zen.
    Crypto / Build: Added AES-BS.
    Debloat: Stripped unnecessary components (some may still be needed for your specific use case).
    Security: Debloated some security features (thanks to physwizz).

Goal:
Push the MT6739 hardware as hard as possible for performance, trading off some compatibility/universality and battery life.

Thanks:
Opencode, DeepSeek, Claude, ChatGPT, kettle, coffee machine, physwizz.
Links

  Base kernel (physwizz): https://github.com/physwizz/a022f-u2.git
