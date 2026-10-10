# Reply-Card + Prompt-Card Adversarial Audit — v0.5.29 → v0.5.30

Adversarial pass over `firmware/hermes-watch/hermes-watch.ino` (v0.5.29, ~2420 lines).
Every finding was verified against the source; every P0/P1 ship was compiled
(`esp32:esp32:esp32s3 huge_app/PSRAM=opi`, clean, 1.26 MB / 40%).

Line numbers below cite the PATCHED file unless marked [0.5.29] (pre-patch).

## SHIPPED (P0 — correctness, order applied)

1. **F1 — "HOLD B LATER" hid nothing; defer did a double flash** [0.5.29
   `deferPrompt()` ~1005-1020]. `deferPrompt` cleared `s_promptCardPainted` but
   left `s_promptOpen == true`, so the render arm slot (`s_promptOpen &&
   !s_promptCardPainted`) re-armed + re-painted the card on the NEXT pass
   (sub-second): the card "blinked", never left — and the +45 s re-raise then
   full-cleared + repainted an already-visible card (double flash). Fix:
   `s_promptOpen = false` on defer (s_hubWaiting deliberately left true so the
   WATCHING-ON-USER ring + watchdog stand-down survive). Now + a v0.5.29
   leftover: `deferPrompt`'s disc clear is followed by `drawStatusLineCol`
   writing into the same erased band cell — reordered so the wipe lands first.
   → hermes-watch.ino ~1005-1030.

2. **F2 — prompt teardown/withdrawal black-holed the panel** [0.5.29
   `promptTeardown`]: the disc wipe erased a concurrently-shown reply card's
   pixels but left `s_replyCardPainted` true, so the eyes slot skipped
   `avatar.update()` AND the card repaint → black panel, no eyes, no card,
   until the stale 12 s timer expired. Fix: `s_replyCardPainted = false` in
   teardown; the reply arm slot re-paints the still-shown card (fresh) on the
   next pass. → ~1029.

3. **F3 — "prompt.close" never reached its handler (card stranded forever
   after hub withdrawal)** [0.5.29 dispatchJson]: `strstr(buf,"\"type\":\"prompt\"")`
   matches `"prompt.close"` too, and the close handler sat 30 lines further
   down — unreachable. A withdrawn approval stayed on wrist indefinitely.
   Fix: exact-type dispatch block (memcmp of the quoted type value, close
   first); removed the dead duplicate handler. → ~1838-1895.

4. **F4 — live-delta repaints punched black holes around the ring**
   [0.5.29 drawPromptCard/drawReplyCard]: every card repaint (prompt: every
   pass before the repaint-once flag; reply: page flips — and now live deltas
   for F5) began with `drawArc(205..230, BLACK)` + two `fillRect` band wipes.
   On the ring's amber/red buckets the arc re-wipe reads as a flicker halo;
   the band wipes re-flash text bands inside the r196 ring. Fix: all wipes
   (arc + status/delta/ticker/label bands + flag resets `s_deltaLive`,
   `s_tickerLive`/`s_tickerDirty`) moved OUT of the draw functions into the
   card arm slots in `loop()` — wipe once per show/flip, keep per-repaint SPI
   at only the text layer. → drawPromptCard ~1036 (body = art only), arm slots
   ~2495-2530.

5. **F5 — status + delta bands bleed next to the card** [0.5.29]: the cards
   wipe/draw their own disc but the PROMPT-era status line (y 38..57) and the
   y=367 delta strip survived above/below the r180 disc (pre-0.5.29 bug),
   and the 0.5.29 arm wipe made the ARM pass flicker them. Fix: both bands
   wiped in the same arm slot as F4, plus the delta-flag resets there so a
   mid-stream `reply.delta` can't make drawTicker/deltaDraw repaint the band
   around the card. → covered by 4 + the `s_replyCardShown` arm slots.

## SHIPPED (P1 — visible polish)

6. **F6 — 27+ char prompt titles clipped both sides**: 18pt caps ≈ 18 px/char
   ⇒ a 27-char title draws ~364 px, but the chord at ink rows 152..181 is
   ~291 px (x ~88..379). Worst 24-char titles ("APPROVE GIT PUSH ORIGIN/…")
   clipped both sides. Fix (drawPromptCard ~1046): wrap once on spaces to ≤16
   chars/row, 2nd row at y 176 (ends ~194, body at 196 still misses it);
   no-space/long titles fall back to 9pt (never clipped).

7. **F7 — "A YES" glyph clipped by the dead corner**: 18pt 'A YES' ~78 px at
   x 90..168 vs chord ~254 px (x 106..360) at ink rows 321..348 — the 'A'
   corner sat ~16 px past the chord over the r196 ring. 12pt bold answers
   (~60 px) at x 106 / 316: inside, and the pair visually centers (B right
   edge ~356). → ~1103.

8. **F8 — 27-char prompt body rows grazed the chord**: 36-char-cap rows with
   space-break produce ≤27-char rows (~238 px) vs ~190 px half-chord at the
   deeper rows. Body capped at 30 chars/row (~183 px worst), and ALL scans
   are now bounded by the 160-char text buffer — a long space-less prompt
   overran its own buffer (read char 60+ into the title region) drawing 4
   rows of ghost title text on the live card. → ~1080.

9. **F9 — both reply hints on the SAME row collided at a page flip**: when a
   delta flipped the card to its last page mid-read, 'A DISMISS - HOLD B
   TALK' (y=320) overpainted 'CLICK B NEXT PAGE' (also y=320) without erasing
   first (two center-datum 21-char strings → merged glyph mush). Hints split
   to 320 + 402 (non-overlapping in both live cases). → ~1220.

10. **F10 — stale '10/10' page-tag glyph columns in the ring**: after
    teardown a '10/10'-era tag was never erased (fixed top_left x=330 draws
    to x~366 > SAFE halfw 174 at y 95..107); shorter '1/1' successors left
    ghost columns. Tag now right-pinned at 354 (top_right datum): '10/10'
    spans 318..354 (SAFE), '1/1' self-compacts to ~332. → ~1170.

11. **F11 — REPLY card was fine print** [0.5.29]: 8 rows × 34 chars × 9pt on
    a 466 px round face = unreadable at arm's length. Rewritten (drawReplyCard
    ~1200): 3 headline rows at FreeSansBold 24pt (chars ≤12, TrueType
    — no bitmap upscale) + 4 continuation rows at 9pt (page capacity 36→
    3*12+4*34 = 172 chars). Cyan 0x00E5FF headline; hints to the bottom rings.

12. **F12 — fixed 272-char page strides SILENTLY DROPPED reply text** (the
    "beyond a wrap fix" bug, invisible to 0.5.29 QA): word-wrap consumes
    variable chars/row (a break-at-space row eats <34), but pages were
    computed as `page * 272`. On a multi-page reply each page boundary could
    skip up to ~24 chars of the wrapped tail. Fix: `buildReplyPages()`
    derives every page's start offset from the actual wrap
    (kReplyHeadCap=12 ×3 + kReplyRowCap=34 ×4, deterministic), recomputed on
    every reply/delta change and used by the page-flip handler too. → ~1110
    + call sites ~1202, ~1945, ~1965ish.

## SHIPPED (P1 — interaction)

13. **F13 — card teardown→eyes had a ~1.3 s black hole** [0.5.29]: answer/
    defer/expire all `fillCircle(disc)` + `avatar.invalidate()` — which sets
    `requiresFullClear_`, so the next `avatar.update()` erased the WHOLE
    panel again (eyes absent for the full transition ~1.3 s). Fix:
    `AvatarEngine::invalidateKeepArt()` (avatar_engine.h:116 +
    avatar_engine.cpp:956: forceRender_ WITHOUT requiresFullClear_) →
    tear-down paths repaint eyes via dirty rects only; panel never goes
    eyes-less. Applied in promptTeardown, deferPrompt, and the prompt arm
    slot (keepArt: the pre-card eyes wipe + full re-render was also visible
    as a blink on every prompt RE-show). 3 sites in hermes-watch.ino.

14. **F14 — reply card showed a DEAD snapshot** [0.5.29]: the hub streams the
    live delta DURING the voice read; the card pinned its full-re text
    (minutes stale by page 3) and had no keep-alive (deltas hit the `else`
    → suppress-only branch). Fix: while `s_replyCardShown`, an incoming
    reply.delta re-extracts into the card buffer, rebuilds pagination,
    repaints once per delta batch (`s_replyRepaint = s_replyCardPainted`)
    and refreshes the 12 s timer (live activity keeps the card alive; the
    auto-dismiss gap is gone). → ~1876.

15. **F15 — fresh reply nuked a card you were reading**: `s_replyCardShown =
    false` unconditional → a painted card whose timer kept running (or
    zeroed: dead buttons/timer). Now: `s_replyCardShown = s_replyCardShown ||
    s_replyLen > 0` (card already up → keep shown; the new text + fresh
    delta stream take over the same card). → ~1944.

16. **F16 — no haptics at all** (the device HAS a vibe motor — the official
    StopWatch UserDemo drives IoeM5PM1 @0x4F reg 0x1B, 12-bit PWM + enable
    bit 15; hermes-watch had zero vibration code). Shipped (raw
    M5.In_I2C.writeRegister — no M5IOE1 dependency): `vibrate(strength, ms)`
    + `vibeTick()` (non-blocking auto-stop) + `setupVibration()` probe.
    Haptics: prompt answer tick (8/255, 50 ms A / same B), reply page
    (6/255, 40 ms), dismiss→talk (6, 24). → ~517-549, setup ~2168, sites
    ~1489/1516/1533.

17. **F17 — a prompt arriving while a reply card was up is INVISIBLE**
    (render chain: prompt arm sits BEHIND the reply-card repaint slot — with
    0.5.29's frozen cards the prompt could wait indefinitely while the reply
    card sat up, and the WATCHING ring was masked). Fix: fresh-prompt +
    re-raise paths now drop the reply card (state + timer + auto-pending)
    AND kill any armed-prompt's live-delta keep-alive — prompt outranks the
    reply card. → ~1885, ~2518.

## NOT SHIPPED (P2, deliberate)

- **`uiText` string-datum drawString leaves glyph ghosts on every re-draw
  (no `setClipRect`)**: real, systemic; proper fix = eraseRect helper keyed
  to each datum/fontheight pair. Left as its own commit — needs a text-metrics
  harness pass over all 14 uiText call sites (would otherwise mix an
  unreviewed refactor into this hotfix set).
- **F4 (per-pass repaint) also starved of 100 ms ring-bucket redraws during
  the old TTL-expiry repaint-once window** and similar SPI bursts: covered
  by the F4 arm-slot change; monitor with the existing `[avatar perf]`
  serial metrics on-flash.

## Verified
- `arduino-cli compile --fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app,
  PSRAM=opi,FlashSize=16M,USBMode=hwcdc,CDCOnBoot=cdc,DebugLevel=error` →
  clean, 1259615 B / 40% flash, RAM unchanged.
- No remaining `0x???`-style raw RGB565 hand-conversions introduced; colors
  are 24-bit passed through LGFX's own 565 conversion (existing convention).