# CRT Sync Debug Plan

## Problem
- Original Groovy core (Groovy_MiSTer-main): ✓ Works on CRT
- Our modified Groovy core: ✗ Doesn't sync (black screen / garbled OSD)
- Timing analysis: ✓ Passing (no violations)
- Not caused by: Switchres header in DDR3, MP4 mode setting

## Hypothesis
One of our code modifications is functionally breaking video output.

## Test Plan

### Test 1: Revert Switchres Changes Only
Temporarily disable just the Switchres additions to isolate if that's the problem.

**In Groovy.sv, comment out:**
```verilog
// Comment these lines:
// input         CMD_SWITCHRES_MP4,
// input  [31:0] SWITCHRES_FRAME_MP4,

// Change these back to original:
wire cmd_init, cmd_switchres, cmd_blit, cmd_logo, cmd_audio, cmd_blit_lz4, cmd_blit_vsync;
wire [31:0] lz4_size, switchres_frame;

// Remove these combined wire assignments:
// wire cmd_switchres = cmd_switchres_hps | CMD_SWITCHRES_MP4;
// wire [31:0] switchres_frame = CMD_SWITCHRES_MP4 ? SWITCHRES_FRAME_MP4 : switchres_frame_hps;

// In hps_ext instantiation, change back:
.cmd_switchres(cmd_switchres),
.switchres_frame(switchres_frame),
```

**In sys_top.v, remove:**
```verilog
// Comment out:
// wire cmd_switchres_mp4;
// wire [31:0] switchres_frame_mp4;
// .CMD_SWITCHRES_MP4(cmd_switchres_mp4),
// .SWITCHRES_FRAME_MP4(switchres_frame_mp4),
```

**In mp4_ctrl_regs.v, comment out:**
```verilog
// output reg cmd_switchres_mp4,
// output reg [31:0] switchres_frame_mp4,
```

**Compile and test.** If this works, the problem is the Switchres logic. If not, the problem is earlier modifications.

---

### Test 2: Check FB_EN Default State
Maybe FB_EN is defaulting to 1 (enabled) instead of 0.

**Add debug to Groovy.sv:**
```verilog
// After line 215 (assign FB_EN = status[60]):
assign FB_EN = 1'b0;  // Force FB_EN to 0 for testing
```

This disables framebuffer mode entirely. If CRT syncs now, the problem is FB_EN or framebuffer-related.

---

### Test 3: Compare QSF Settings
Check if Groovy.qsf differs from original:
```bash
diff Groovy.qsf Groovy_MiSTer-main/Groovy.qsf
```

Look for differences in:
- MISTER_FB macro
- Clock settings
- Pin assignments

---

### Test 4: Binary Search Our Changes
If none of the above works, use git to bisect which change broke it:
```bash
# Find the commit that broke CRT sync
git bisect start
git bisect bad HEAD  # Current (broken)
git bisect good d31f543  # Initial commit (should work)
# Git will checkout middle commits - compile and test each
```

---

## Expected Outcome
One of these tests will identify the specific change causing the CRT sync issue.
