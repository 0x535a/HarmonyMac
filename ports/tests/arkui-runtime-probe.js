// Verify original ACE's real framework globals; do not define substitutes.
globalThis.__auroraArkProbe =
    (typeof Observed === 'function' ? 1 : 0) |
    (typeof Track === 'function' ? 2 : 0) |
    (typeof AppStorage !== 'undefined' ? 4 : 0) |
    (typeof ViewPU !== 'undefined' ? 8 : 0) |
    (typeof loadDocument === 'function' ? 16 : 0);
