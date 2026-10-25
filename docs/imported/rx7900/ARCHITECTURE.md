<!--
Module: rx7900.documentation.architecture
Purpose: Describe native ownership, reusable GPU components and verified limits.
Created: 2026-10-21
Adapted from: Sonic-SNES ARCHITECTURE.md; documentation conventions only.
-->
# Architecture

## Overview

RX7900-ReactOS is a C++17 bring-up project for Radeon RX 7900 XTX/Navi31. The demand-start RX7900Probe service and installer provide read-only PCI inventory using real ReactOS NT/HAL APIs. Devices 1002:7448/744A/744B/744C/745E identify Navi31 GPU functions; subsystem/revision and the user's board identification distinguish the actual card. The separate experimental rx7900video miniport implements VideoPort callbacks and checked mode IOCTLs, maps its assigned BAR5 through VideoPort, runs physical ASIC firmware loading only when its service key sets PhysicalAsicInit=1, and refuses initialization until actual display prerequisites exist. No INF automatically replaces the working display driver. Linux-derived components prepare SDMA6 packets, GFX11 system PTEs, checksummed IP-discovery bases, flat VMID0 GART configuration/TLB acknowledgements, IH6 events, ATOM connector routes, EDID timings and disabled DCN3.2 OTG register writes/readback. Memory and queue lifetimes retain or quarantine uncertain hardware resources. Native firmware file acquisition, DMA common-buffer ownership and bounded MMIO services compile against the ReactOS SDK. Firmware container validation is neither authentication nor GPU upload. A bounded PSP13 bootloader protocol retains aligned MC staging, checks ready/completion transitions and quarantines uncertain submissions. The physical ASIC sequencer follows the Linux v6.12 order: VRAM IP-discovery and PSP runtime-database reads through MM_INDEX/MM_DATA, NBIO4.3 strap and HDP remap, MMHUB3.0 VRAM placement, SOS boot, PSP13 KM ring, TOC-sized TMR, PMFW before SETUP_TMR, RS64 CP/RLC/MES/IMU/SDMA/DMCUB payloads with RLC autoload, ASD and RL, then SMU13 version checks and table locations, using an uncached 32 MiB BAR0 VRAM arena at VRAM offset 64 MiB. Around those steps it follows amdgpu_device_ip_init: GMC11 MMHUB/GART with the VRAM scratch and dummy pages, two-stage PSP DRAM training with bottom-of-VRAM save/restore when ATOM firmwareinfo and the runtime database request it, IH6 rings before firmware load, SMU13 PPTable (BoardTable append without SCPM), BTC, allowed/enabled features, default DPM tables from PMFW with ATOM boot-clock fallbacks, PCIe override and THM/MP1 thermal alert, then GFX11 RLC autoload wait, RS64 start, GFXHUB, golden/constant registers, clear state and CP start, MES11 KIQ with scheduler map/SET_HW_RESOURCES, eight compute and one gfx kernel queue mapped through MES with ring tests, SDMA6 kernel rings with doorbell ring tests, the soc21 late-init doorbell self-ring aperture, SMU late init (power limits, workload profile), the MES self-test (a diagnostic Linux ignores), soc21 clock/power gating with GFXOFF, and the amdgpu_ib_ring_tests IB tests. IH, kernel rings, EOP, writeback and MQDs live in GTT: DMA system pages bound through the GART with Linux PTE flags. An IH6 ISR/DPC service is armed right after IH hw_init (Linux IRQ-handler order) and enables the CP/SDMA/VM fault sources afterwards. AsicInit::shutdown runs amdgpu_device_ip_fini_early (ungate, SMU fini, reverse hw_fini) and an explicit mode1_reset follows amdgpu_device_mode1_reset and is the reset proof that releases every lease; neither runs implicitly. It has run only against synthetic devices; IP versions and bases were checked against the user's board's sysfs discovery listing, not by running on it. With the PhysicalAsicInit opt-in the VBIOS is read from the card in Linux amdgpu_get_bios order (legacy shadow, ROM BAR, SMUIO register read; ACPI ATRM/VFCT are unavailable to a ReactOS miniport) with the administrator rx7900.rom as last resort or explicit AtomRomOverride; without the opt-in only the file is read. Every image is checked against the assigned PCI vendor/device; this is not board authentication. The SMU PCIe link follows amdgpu_device_get_pcie_info over the real upstream bridges (Gen4 x8 on the user's machine). Current ReactOS cannot reach PCI extended config space, resize BARs or deliver MSI to a miniport; a HAL ECAM proposal is in patches/rebar/ and is not applied. The physical Radeon OpenGL stack now includes a loadable ReactOS WGL ICD (src/opengl/icd) that opengl32 loads from the OpenGLDrivers key: it exports the Drv* entry points, installs the exact 336-entry GL 1.1 dispatch table from DrvSetContext and renders through a portable OpenGL 1.1 state machine (gl_context: immediate mode, vertex arrays, display lists, matrix stacks, lighting, fog, 2D texturing, clipping, blending, alpha/depth/stencil test, scissor, logic op), whose software reference rasterizer (gl_raster, BGRA8) is presented to the caller's HDC with StretchDIBits on DrvSwapBuffers. The shipped renderer is software; no hardware acceleration is claimed. The GFX11 pipeline foundation adds a float/exp/NGG graphics subset to the gfx11_isa encoder (byte-exact against pinned LLVM 18.1.8 vectors) and a state-keyed fixed-function vertex/pixel shader generator (gl_shader_gfx11); the kernel-safe generator now emits merged NGG ES+GS shaders (GS_ALLOC_REQ, primitive and pos0 exports, attribute-ring stores) for points, lines and triangles, object-space transform/lighting/fog, and pixel shaders with lds_param_load/VINTERP interpolation, image_sample_lz texturing, all texenv modes, fog and alpha test, against the versioned gl_gfx11_abi.hpp (v3). A graphics draw model in the interpreter (run_draw) pixel-checks them against gl_raster; Process ABI v3 adds graphics_open/submit/query and bulk upload/readback: the ICD sends a typed draw description (handles, GL enumerants, float bits, ShaderKey, vertex records); the kernel validates it, generates shaders into driver-owned read-only GTT, builds the PM4 IB through a register allow-list validated against the GC 11.0.0 headers, and runs it on the session's MES gfx user queue with fence-held leases. Gfx11Backend plugs into Context with software migration for stipple, FRONT_AND_BACK, DrawPixels/Bitmap/CopyPixels and oversize surfaces. Present goes through radeondisp DrvDrawEscape (RGLP) to a kernel DMA_DATA copy into the adopted scanout under win32k's CLIPOBJ, with exact GPU readback plus SetDIBitsToDevice as fallback; the ICD reaches the process service through DrvEscape (RGLQ). All of it is host/Wine-tested against gl_raster through a PM4 pipeline model; nothing has run on the card. It is not yet a hardware-accelerated WGL ICD. An isolated trusted GFX kernel-ring submission owner retains exact shader, vertex and render-target slices until a real FenceManager receipt or confirmed queue/reset retirement, reusing the queue's existing IB lease; the production bridge has no native caller or complete draw producer and is not packaged. Its 39-check host ASan/UBSan and GCC13 i386 Wine fixtures pass, and the GCC13 AMD64 fixture compiles; MSVC2022 and physical execution remain untested. AsicInit::suspend/resume follow amdgpu_device_suspend/resume from the miniport power hooks; an unposted card is re-POSTed on resume (and after mode-1 reset recovery) through the bounded ATOM interpreter running the VBIOS ASIC_Init table (amdgpu_device_asic_init for GC 11); a failed re-POST leaves it suspended and retryable. The interpreter matches an independent atom.c reference on 3000 generated programs and survives 24000 mutated images under ASan; hardware confirmation with a real Navi31 VBIOS is pending. With PhysicalAsicInit=1 the miniport can adopt the firmware's UEFI/GOP or VBE linear DCN3.2 scanout read-only as its single mode (legacy VGA text/planar modes are refused; after a legacy BIOS/CSM boot, VbeModeSet=1 first asks the VBIOS for a VBE linear 32 bpp mode, host-tested only) and, in RX7900_RADEON_2D builds with Radeon2D=1, serves private Radeon 2D IOCTLs (opaque handles, no addresses) through SDMA1 subwindow copy and constant fill to the radeondisp GDI DLL; offscreen surfaces are evicted to GTT before suspend and restored after resume; clip lists over 64 rectangles are batched in band order; NT DrvCreateDeviceBitmap device bitmaps are implemented, though the pinned ReactOS win32k never calls it. A GFX11 compute raster engine (ROP3/ROP4, patterns, mono and coverage8 glyphs, colour key, nearest stretch, AlphaBlend, gradients including single-colour and unbounded-height triangles, cosmetic lines, 1/8 bpp operands) is host-tested pixel-exact against the GDI engine in an ISA interpreter and, with Radeon2DCompute=1, serves raster IOCTLs 0xa10-0xa16 (ABI v3) to radeondisp DrvBitBlt/CopyBits/TransparentBlt/AlphaBlend/GradientFill/RealizeBrush/LineTo, with fence ordering between SDMA and compute. Same-surface ROP and guarded stretch now preflight actual clipped writes against the conservative live source region; any real overlap retains the engine snapshot fallback. The pinned ReactOS win32k never calls DrvTextOut, DrvStrokePath, DrvStretchBlt, DrvFillPath or DrvCreateDeviceBitmap, and with the optional stretch-guard win32k patch the paired line helper now also tags unstyled lines for radeondisp (fixed identity escape in radeon_line_identity.hpp), so solid cosmetic lines reach the compute line kernel; without that patch lines stay in software. With the same patch radeondisp hooks DrvStretchBltROP (ABI v4 0xa17): guard-unwrapped mirrored nearest stretch with ROP4, solid or 8x8 brush, 1-bpp mask and mono sources, reproducing IntEngStretchBlt/EngStretchBltROP geometry; unguarded or inexpressible calls return FALSE to the engine's own fallback and are counted. The pinned engine draws pattern pens as their iSolidColor (EngLineTo), so tagged pattern pens are drawn the same way; the tiled line kernel path exists but has no ReactOS caller. With HardwareCursor=1 the adopted pipe's DCN3.2 cursor (64x64, Linux v6.12 dcn32 hubp/dpp and MPC VUPDATE lock) serves the VideoPort pointer IOCTLs; host-tested only. A portable DisplayPort SST probe performs bounded AUX/DPCD reads and I2C-over-AUX EDID acquisition through an owner-supplied callback, then reuses the strict sink decoder and RGB8 mode budget; a bounded 8b/10b source/sink training policy also handles clock recovery, equalization and failed cleanup. A read-only native HPD adapter uses a validated DCN3.2 DMU segment2 and the miniport-owned BAR5 mapping under an external pin/epoch/lock proof. A bounded five-engine DCN3.2 AUX transaction backend now implements owner-gated engine enable/reset, native request framing, arbitration, completion and reply parsing through the same owned BAR5 mapping, with permanent quarantine on uncertain completion/release. Its one-shot native adapter requires external proof of the exact DDC GPIO hardware AUX route, clocks/PHY, DMUB arbitration and exclusive epoch before use. The opt-in DCN 3.2 link owner (RX7900_DISPLAY_LINK, registry DisplayLinkOwner=1 with PhysicalAsicInit=1; output/display_link) now supplies that proof: ATOM connector paths bind to UNIPHY A..E, DIG engine, DDC/AUX line and HPD pin only when the DC_GPIO indices match the discovered DMU 3.2.0 segment2 base (Linux link_factory/bios_parser2/hw_translate_dcn32), with duplicates, USB-C, eDP and external encoders quarantined; the DDC pad is switched to hardware AUX mode and read back as hw_ddc does. It owns SMU13 DALSMC hard minimums, the DCN32 clock table and DCCG DP/DPP DTOs and dividers (dcn32_clk_mgr order, raise-before/lower-after), DIO link-encoder init and the PHY through the VBIOS DIG1TransmitterControl v1.6 and SetPixelClock v1.7 tables on the CPU ATOM interpreter (Linux's non-DMUB command-table path), DP training through the existing policy, HDMI/DVI TMDS with DC_I2C-over-DDC SCDC scrambling and 1/40 bit-clock ratio above 340 MHz, and DMCUB start (CW3..CW6, inboxes, boot status) with a bounded inbox1 ring; every uncertain write quarantines until a reset proof. The stream side (OTG/plane/commit) is dcn-stream's. All of it is host/Wine-tested against synthetic register, PMFW, VBIOS-table, DPCD, SCDC and DMCUB models only; video.cpp does not call it yet, no physical link has been trained and no new mode is advertised. Physical execution of the cursor sequence, DMUB start, VCN, native DDC/AUX and PHY, clocks/bandwidth, owned scanout and native OpenGL integration remain unverified on hardware; the video.cpp link-owner hook, bandwidth/watermarks and the stream commit remain unfinished. The native process service now links into rx7900video and owns referenced NT sessions, authorized VRAM/GTT buffers and MES queues; after a confirmed destructive reset it refuses reinitialization until all old owners are retired and a new ASIC configuration is ready, preserving monotonic handles. A version-2 native process API accepts semantic SDMA copy/fill work, builds driver-owned mapped IBs and retains ProcessMemory buffer rights through actual queue fences or proven queue/VM retirement; it exposes no raw command streams and preserves version-1 lifecycle requests. The synthetic engine/page-table submission fixture passes 45 ASan/UBSan checks, and rx7900video links on GCC13 i386/AMD64. Exited sessions are reaped by a reaper thread within a bounded wait (MES REMOVE_QUEUE preemption, GPUVA unmap with acknowledged flush, VM teardown; a failed step quarantines and escalates once to forced recovery), and after a confirmed mode-1 reset live sessions are rebuilt with new PASIDs and VMs at their recorded GPUVAs, GTT contents kept, VRAM buffers reported lost once (ABI v4 buffer_lost/work_lost/session_lost) and fences continued; host-tested only. The Radeon DDI now accepts one source-pinned styled cosmetic segment through a guarded DrvStrokePath and one 8-bpp FreeType coverage glyph through a read-only identity plus kernel-only DrvNotify, reusing the existing GFX11 line and text IOCTLs; unsupported cases leave the destination untouched. The opt-in win32k caller bridges now reach them: reactos-radeon-bridge.patch calls DrvStrokePath for an identified radeondisp surface (identity request bit flag_styled_stroke) before DrvLineTo, and reactos-radeon-text-bridge.patch routes FreeType coverage glyphs to DrvNotify (DC_TRIVIAL/empty clips decline); both apply to a97c8a2 and 822e864 and are compile/Wine-tested only. DrvTextOut (STROBJ/FONTOBJ enumeration are stubs), DrvFillPath (paths become regions through DrvBitBlt) stay unbridged by design. The opt-in reactos-radeon-devbitmap.patch makes compatible bitmaps radeondisp device bitmaps (identity bit flag_device_bitmaps): DrvDeleteDeviceBitmap from SURFACE_vCleanup exactly once, every direct pixel reader/writer (Get/SetBitmapBits, BITMAP_CopyBitmap, DIB transfers, flood fill, cursor data) converted through DrvCopyBits under the devlock, and conversion before dynamic mode changes; compile- and Wine-tested only. ABI v5 makes the remaining raster cases exact on the GPU: guarded stretch whose device window leaves the source (the engine's temporary is zero-filled by GDIOBJ_AllocateObject, reproduced by a scratch snapshot), negative source extents (C truncation via |w| plus a flipped step), tile plus mask with the DIB running pattern phase, same-surface windows meeting the output (SDMA snapshot), 8/16/24 bpp sources through staged XLATEOBJ tables, column-strip staging, and styled lines under complex clips. Only exact engine fallbacks remain declined (small rgb16 stretches, mask-edge samples where 822e864 reads past the bitmap, styled lines over 2048 steps, insufficient staging). Device bitmaps, surface recovery after reset and installation remain 2D gaps. No physical display or acceleration operation has been verified.

Modules own distinct responsibilities and reuse their public contracts. src/common owns checked binary views, PCI decoding, BIOS validation and the pointer-free diagnostic ABI. src/hardware owns firmware container layouts and the PSP payload catalog, shared checksummed IP discovery and its VRAM acquisition, the PSP mailbox and KM-ring protocols, NBIO/HDP and doorbells, the SMU13 mailbox/power-play setup, ATOM firmwareinfo, PSP DRAM training, GMC11 VM hubs, IH6, GFX11 core and kernel queues, MES11, SDMA6 kernel rings, clock gating, the IH6 interrupt service and the ordered ASIC sequencer behind one register-callback contract and one doorbell-callback contract. src/memory owns allocation handles, backing/quarantine lifetimes, PTEs and the narrow GFXHUB protocol; actual native callbacks must establish DMA/GPUVM visibility. src/acceleration owns SDMA encoding, queue publication/completion and IH decoding. src/display owns timing/scanout validation, confirmed mode transactions and inactive OTG programming; src/output owns sink/board discovery. src/opengl reuses memory handles and the SDMA byte writer for checked graphics packet construction, the gfx11_isa encoder for shader generation, and owns the portable GL 1.1 context, software rasterizer and the ReactOS WGL ICD. src/reactos owns NT/VideoPort entry points, aligned extension construction, real mappings, reusable firmware/ROM file ownership, DMA adapters and the opt-in ASIC adapter (BAR0 VRAM arena, MMIO and timing callbacks, no reset owner); no user API accepts arbitrary register or GPU addresses. src/tools reuses bounded file I/O and PCI formatting. All authored implementation and tests are C++17; CMake/JSON are build inputs. GCC13 and MSVC2022 are compiler targets; MSVC is unavailable and untested. Kernel code allocates through native APIs, disables exceptions/RTTI, and uses nonallocating placement construction for object lifetime. No Sonic runtime code is reused. Build scratch belongs in Binaries/.build, published packages in Binaries.

Configure host checks with cmake -S . -B Binaries/.build/host -DRX7900_SANITIZERS=ON, build, then run ASAN_OPTIONS=detect_leaks=0 ctest --test-dir Binaries/.build/host --output-on-failure. LeakSanitizer alone is disabled because sandbox tracing prevents it. Sixty-six host test groups pass with ASan/UBSan (the new opengl_context and opengl_shader groups among them); GCC13 i386/AMD64 builds run, the twenty-four physical-initialization groups also pass as GCC13 MinGW i386/AMD64 executables under Wine, and MSVC2022 remains untested. Optional RX7900_LIGHTNING_TRANSPORT_TIMING adds fixed device-owned QPC aggregates across all lightningnt5 consumers, with bounded dumps on validated context close or lifecycle boundaries. It leaves waits, fences, IRQ policy and wire ABI unchanged; default-off builds have no QPC import. The production sampler passes26 ASan/UBSan checks and GCC13 i386/AMD64 fixture builds; the actual transport passes exact i386 SDK compilation ON/OFF. Native frame-time attribution has not yet been measured. Native physical-initialization sources pass a GCC13 syntax-only compile with the exact ReactOS i386 SDK flags; the shared ReactOS tree was not rebuilt for them. Integrate with cmake -DREACTOS_SOURCE=/path/to/reactos -P cmake/integrate.cmake. Optional REACTOS_APPLY_PCI_FIX, REACTOS_APPLY_FRAMEBUFFER_SHADOW and REACTOS_APPLY_CURSOR_RECOVERY apply checked, idempotent native integration patches. REACTOS_APPLY_CLIP_VISIBILITY applies the independent empty-region patch; explicitly enable RX7900_FRAMEBUFFER_CLIP_VISIBILITY and set RX7900_FRAMEBUFFER_CLIP_VISIBILITY_SOURCE to src/reactos when building the exact guest release. Its 31 public-DDI checks pass under GCC13 i386/AMD64 and Wine; that does not establish interactive refresh correctness. After applying the cursor recovery patch, explicitly enable RX7900_FRAMEBUFFER_CURSOR_RECOVERY and set RX7900_FRAMEBUFFER_CURSOR_RECOVERY_SOURCE when building the exact OS release; the installer never substitutes a newer SDK Win32k binary. Enable the cached adapter separately with RX7900_FRAMEBUFFER_SHADOW and its source path in the native build. The demand-start diagnostic installer passed install/load/IOCTL/unload/reload/removal tests. The canonical existing QEMU-lightning11.1.2 executable and host renderer were rebuilt in place; the ReactOS0.4.16 i386 guest boots with its normal Lightning64-bit BAR using the patched PCI resource snapshot/probe serialization. Native NT5 transport capability exchange passed. The recovery i386 VM selects Lightning and executes GDI fill, overlap copy, upload and blending on the existing host GL context; its current143-check pixel, text, mode-cycle and publication run passes. The app-local i386 OpenGL runtime now renders the unchanged original wglgears through Zink, Venus and the host Radeon GPU. Native PDO/filter registration, opaque process-owned GPU image leases, real producer fences, current DC clipping, GPU desktop composition and source-read receipts are integrated. The private NT5 condition-variable owner prevents lost wakeups; the Win32 WSI acquire/present lock inversion and terminal-failure wakeup are corrected. Actual maximized 1920x1033 gears pixels, advancing external-image GPU counters, zero GL error and normal close/reopen pass on the installed runtime; this is not a sustained FPS or physical-vblank result. The corrected stock-hatch mask integration now passes the actual 139-check GDI run. A subsequent fresh windowed GPU launch exposed a 256-byte linear DMA-BUF pitch requirement and live ring-mapping revocation during quarantine. The export/copy pitch and borrowed-view retention are corrected, and the actual small-window original now submits GPU images. On the old kernel a fully covered GPU window painted through the active browser: IntEngUpdateClipRegion classified an explicitly empty region as unbounded DC_TRIVIAL. The C++ empty-region helper changes only explicitly updated empty clips to DC_RECT; initialized unbounded clips retain their semantics. The first empty-region kernel alone did not resolve activation overdraw. DceResetActiveDCEs skips foreign process-owned DCs during activation; its TIF_MOVESIZETRACKING PID bypass also allowed unsafe foreign region-attribute reads during resize. A paired C++ owner gate now defers foreign visibility work, refreshes it in the actual owner before DrawEscape locks its DC/PDEV, and preserves pending work before UserGetDCEx overwrites transient flags. The exact prior GPU activation case now passes. Shared C++ USER ownership also spans the unchanged SetDIBits/StretchDIBits bodies for retained software HDCs; combined exact-release kernel806d94 is installed with tracing disabled, and the broader guest checks remain pending. Production owner-gate fixtures pass48 checks each on GCC13 i386/AMD64 and Wine, separate from the31 empty-clip checks. Optional RX7900_DRAW_ESCAPE_CLIP_TRACE logs locked DCE/DC/region metadata; performance is not measured under tracing. Ordinary C-root gears still needs native installed ICD selection/runtime bootstrap to stop choosing software OpenGL; the app-local GPU original has a real50–52FPS throughput gap that requires transport timing attribution. Earlier passing maximized GPU runs do not establish complete recovery. Original standalone copies outside the private LightningGL runtime can still select ReactOS software OpenGL. The guest desktop remains1920x1080 with an absolute USB tablet; the recovery GTK window uses zoom-to-fit for a larger visible view, and GDK reports59980mHz. The opt-in framebuffer adapter draws into an owned cached bitmap and synchronously copies dirty bounds to scanout. Its actual guest test passes35 pixel/lifecycle/counter checks. ABI2 measurements isolate a640x360 fill at0.0383ms cached rendering versus22.7033ms physical flush; this is not a60Hz result. The guest release822e864 has unimplemented PAT initialization and maps requested WC memory to a UC-minus PAT entry; a paired, exact-release boot/PTE correction is under review, with no live driver PAT/MTRR writes. The native Lightning display path has bounded partial uploads, a cursor plane and paced console publication in the same current source/build. The live display client retains a generation-only retirement credential when an accepted owned MAP returns an unusable mapping, so an uncertain release blocks the next MAP instead of losing its RAM lease. Once an authenticated replacement staging bitmap is published, an old-wrapper release failure keeps its exact token pending without hiding that PDEV. GCC13 i386/AMD64 lightningdisp builds and the 48-check production DisplayLink fixtures pass; the host staging-retirement fixture passes 33 ASan/UBSan checks. These checks do not exercise native D3/GTK or the VM; an acknowledgement lost after native release and surprise-removal teardown still retain uncertain RAM. Damage-aware copies into three rotating immutable GPU publication images are implemented in the canonical QEMU build: bounded coalescing and independent image deficits advance only after actual copy completion, BUSY/rejected copies retain damage, and reset/mode changes force fresh baselines. The combined GPU/publication/cursor fixture passes 115 checks; controller/runtime/damage fixtures pass 110/91/233 ASan/UBSan checks. The paired ReactOS run passes 49 checks and records 637 partial versus 43 full publication copies with zero GL errors. Scene mean 16.557 ms and transitions 528.934 ms do not establish a latency improvement or smooth 60 Hz. GTK still renders and swaps its full viewport. Source-pinned text batching reuses the cached surface and existing glyph engine. The native cursor owns canonical shape bytes across mode/power transitions, validates exact representability and acknowledges real hide/show publication. Read-only cursor statistics carry an explicit recovery flag; the source-pinned engine reselects the original cursor bitmap after a confirmed live failure and acknowledged hide. The native API run passes 69 checks including mode changes, corners and controlled UI rejection; the production DisplayLink fixture passes 48 checks under GCC13 i386/AMD64 and Wine, with native D3/GTK explicitly outside that fixture. Shared cursor geometry passes 4,303 ASan/UBSan checks. The independent GPU-composited cursor overlay is integrated for managed GTK consumers: shape-only uploads, metadata-only movement, clipped/HiDPI placement, actual frame acknowledgement and context retirement reuse the existing host GL context. Both default XWayland/EGL and native Wayland/GLArea ReactOS runs pass 69 checks, including forced UI rejection and exact engine-owned fallback. An actual window capture verifies the independently positioned cursor texel and transparent neighbors. Native Wayland detached-toplevel share-group replacement is integrated through context-local EGLImage imports and real consumer-EGL-fence to producer-GLsync retirement. The final detached consumer passes 69 guest checks, the reattached consumer passes 67, and actual fullscreen/detach/reattach retains publication without additional rejection or GDI GL error. The final default EGL run also passes 69 checks and handles fullscreen and detach. Native D3 replay is implemented but not exercised by these VM runs. The GDI controller, host GPU backend and native miniport/client share checked typed raster v3 snapshots (144 bytes; restricted v2 compatibility), source-space guarded mirrors, raw pre-XLATE color-key masks and exact integer stretched alpha. All 256 Boolean ROP3 tables, masked ROP4, realized tiles and coverage8/mono glyph composition reuse the existing canvas. Disjoint same-color glyphs aggregate in bounded device-owned CPU coverage before accepted GPU submission; ordering conflicts flush before mutation, CPU staging or readback. The GPU alone executes accepted fills; actual CPU consumers gather the stale canvas through existing readback ownership. Nonoverlapping screen-source alpha retains a GPU snapshot. Solid cosmetic lines use bounded C++ integer coverage and the existing GPU mask compositor; styled or patterned lines remain engine-owned. Paint retains optional monochrome brush masks through public DrvRealizeBrush and uses the existing masked tile compositor, including transparent MIX and checked signed phase. The new 88-byte gradient descriptor carries original 16-bit colors, mesh geometry, triangle origin and extents; an integer GPU shader reproduces the pinned engine's ordered edge/color steps. Native GradientFill and SynchronizeSurface callbacks share the PDEV lock, packet and clip workspace. Source-pinned C++ helpers check gradient capture multiplication, reject empty/out-of-clip solid-triangle spans and give active-primary GdiFlush a real GPU fence barrier without forced readback or premature text-group closure. Native completion polling spends at most 200 microseconds in short polls before yielding under the existing deadline; completion never becomes a fabricated success. The actual GPU fixture passes 593 checks and protocol/controller/runtime fixtures pass 1731/249/142. New line coverage passes 649 ASan/UBSan host checks and 649 each under GCC13 i386/AMD64 plus Wine. The exact-release Win32k and native package build; all 30 display imports exist in the pinned release. The source-pinned FillRgn boundary now uses checked surface brush origins, current DC colors and balanced local brush cleanup; stock hatch masks invert only meaningful ink bits. Content-aware source integration prevents rounded timestamps from retaining old compiled code. The paired1920x1080 guest run passes143 checks with1 synchronized CPU fallback and no GL error. Scene mean41.080ms, p9553.191ms and text mean17.1224ms leave latency unresolved; the new real GdiFlush barrier changes the meaning of earlier submission timings. GPU offload has not established smooth60Hz; original OpenGL gears now uses the separate GPU window path. Lightning masked stretch (capability LIGHTNING_GDI_CAP_STRETCH_MASK, host shader running PatternX phase) and pattern-brush lines (drawn as the engine's solid iSolidColor) are implemented and CPU/Wine-tested; the pinned engine produces neither case, and the host shader change awaits a GL-driver run. GPU offscreen memory-DC resources, clips beyond 64 rectangles, additional stretch modes and styled lines remain implementation gaps. Zero driver fallback counters do not include every CPU engine path. GTK still swaps its full viewport. Native GNU function-frame checks do not prove the complete kernel callback stack safe; MSVC2022 remains untested. This is a GPU overlay, not a physical KMS cursor plane. Console publication completion must not be described as physical monitor vblank. The user's desktop GPU0000:03:00.0 remains bound to Linux amdgpu (1002:744C/C8, subsystem1DA2:471E); physical GPU testing remains deferred. Current SDK baseline a97c8a2cdcf6a9a7f5cc543951a02ccb7a80d8ca; installed VM kernel822e8647792a2c43f033e3e00ed273d59f163c7b. Exact provenance and limits are in THIRD_PARTY.txt and licenses/.

```text
rx7900-reactos/
|-- ARCHITECTURE.md                 Boundaries, status and validation
|-- STYLE_GUIDE.md                  C++17, reuse and memory-safety requirements
|-- CMakeLists.txt                  Host, Windows and native ReactOS targets
|-- CMakePresets.json               GCC13 and untested MSVC2022 toolchains
|-- cmake/
|   |-- integrate.cmake             Owned integration and checked opt-in patches
|   |-- mingw-i386.cmake            GCC13 Win32 toolchain
|   `-- mingw-amd64.cmake           GCC13 AMD64 toolchain
|-- THIRD_PARTY.txt                 Source and test provenance
|-- licenses/
|   |-- AMDGPU-BIOS.txt              BIOS attribution
|   |-- AMDGPU-SDMA.txt              Packets, GPUVM, discovery and IRQ attribution
|   |-- AMDGPU-DISPLAY.txt           Timing and DCN register attribution
|   |-- AMDGPU-OUTPUT.txt            ATOM and EDID attribution
|   |-- AMDGPU-FIRMWARE.txt          Firmware container attribution
|   `-- OPENGL.txt                  GL/GFX11 references and precise limits
|-- src/
|   |-- common/
|   |   |-- byte_view.hpp           Checked binary reads and borrowed subviews
|   |   |-- status.hpp              Common parser errors
|   |   |-- protocol.hpp            Fixed-width diagnostic ABI
|   |   |-- pci.hpp                 PCI identity/BAR contract (Navi31 GPU IDs)
|   |   |-- pci_caps.hpp            PCI capability, PCIe link and save/restore contracts
|   |   |-- pci_caps.cpp            Capability walks, partner link, config save/restore
|   |   |-- pci.cpp                 Shared PCI parser
|   |   |-- atom_bios.hpp           BIOS signature contract
|   |   `-- atom_bios.cpp           Linux-derived signature checks
|   |-- hardware/
|   |   |-- firmware.hpp            Bounded firmware metadata contracts
|   |   |-- firmware.cpp            SDMA/PSP/DMCUB container validation
|   |   |-- discovery.hpp           Checked IP/base provenance contracts
|   |   |-- discovery.cpp           Shared bounded discovery traversal
|   |   |-- psp_boot.hpp            Retained staging/mailbox ownership
|   |   |-- psp_boot.cpp            PSP13 bounded bootloader protocol
|   |   |-- register_io.hpp         Shared bounded register-callback contract
|   |   |-- vram_discovery.hpp      Discovery snapshot/runtime-database contracts
|   |   |-- vram_discovery.cpp      IFWI wait and indirect VRAM reads
|   |   |-- nbio.hpp                NBIO/HDP/SMN/VRAM-placement contracts
|   |   |-- nbio.cpp                Strap, HDP remap/flush and MMHUB placement
|   |   |-- ucode.hpp               PSP payload catalog contracts
|   |   |-- ucode.cpp               GC/RLC/MES/IMU/SMC/SDMA/DMCUB/TA decoding
|   |   |-- psp_ring.hpp            KM ring, command and lease contracts
|   |   |-- psp_ring.cpp            GPCOM frames, fences, TOC/TMR/IP firmware
|   |   |-- smu13.hpp               MP1 mailbox, table and THM contracts
|   |   |-- smu13.cpp               SMU13 messages, PPTable, DPM features, thermal alert
|   |   |-- atom_firmware.hpp       ATOM firmwareinfo/BoardTable contracts
|   |   |-- atom_firmware.cpp       Training capability, boot clocks, smc_dpm_info
|   |   |-- atom_interpreter.hpp/.cpp Bounded ATOM bytecode interpreter (Linux atom.c port)
|   |   |-- psp_training.hpp        Two-stage DRAM training contracts
|   |   |-- psp_training.cpp        PSP long training with VRAM save/restore
|   |   |-- vmhub_layout.hpp        MMHUB3.0/GFXHUB3.0 register layouts
|   |   |-- gmc11.hpp               GART/VM hub contracts
|   |   |-- gmc11.cpp               GART table, hub enable and TLB flush
|   |   |-- ih6.hpp                 IH ring contracts
|   |   |-- ih6.cpp                 OSSSYS6 ring programming and doorbells
|   |   |-- pm4.hpp                 Shared PM4 type-3 header and opcodes
|   |   |-- gfx11_clear_state.hpp   Generated clearstate_gfx11 tables
|   |   |-- gfx11_init.hpp          GC11 RLC/CP core contracts
|   |   |-- gfx11_init.cpp          Autoload wait, RS64, constants, CSB, CP start
|   |   |-- gfx11_queues.hpp        GC11 KGQ/KCQ contracts
|   |   |-- gfx11_queues.cpp        Gfx/compute MQDs, MES mapping, ring and IB tests
|   |   |-- mes11.hpp               MES KIQ/scheduler contracts
|   |   |-- mes11.cpp               KIQ MQD, scheduler map, API frames, queues, self-test
|   |   |-- mes_selftest_vm.hpp     MES self-test process VM contracts
|   |   |-- mes_selftest_vm.cpp     Four-level process page tables and metadata map
|   |   |-- clock_gating.hpp        soc21/GC11 clock and power gating contracts
|   |   |-- clock_gating.cpp        CG/PG gate and ungate, RLC safe mode, GFXOFF
|   |   |-- irq_service.hpp         IH6 ISR/DPC service contracts
|   |   |-- irq_service.cpp         IH drain, IV dispatch, interrupt source enables
|   |   |-- sdma6_init.hpp          SDMA6 kernel ring contracts
|   |   |-- sdma6_init.cpp          Ring start, doorbell range, ring test, halt
|   |   |-- vbios_rom.hpp           VBIOS validity and SMUIO ROM-read contracts
|   |   |-- vbios_rom.cpp           check_atom_bios, ROM image walk, SMUIO read, VFCT match
|   |   |-- psp_ta.hpp              RAS/HDCP/DTM trusted-application contracts
|   |   |-- psp_ta.cpp              TA load/unload, RAS support and boot-config decision
|   |   |-- fence.hpp               Kernel-ring fence and job-timeout contracts
|   |   |-- fence.cpp               Interrupt/fallback fence processing and hangs
|   |   |-- gpuvm.hpp               Process VM, PASID and TLB invalidation contracts
|   |   |-- gpuvm.cpp               Four-level page tables and MES/register flushes
|   |   |-- user_queue.hpp          MES user-queue and PASID contracts
|   |   |-- user_queue.cpp          Process queues, MQDs, doorbells and MES add/remove
|   |   |-- scheduler.hpp           Job scheduler entity/fence contracts
|   |   |-- scheduler.cpp           drm_sched-style credit limit, timeouts and recovery
|   |   |-- blit2d.hpp              SDMA6 2D engine contracts
|   |   |-- blit2d.cpp              Subwindow copy, constant fill, upload/readback IBs
|   |   |-- radeon_2d_backend.hpp   Radeon 2D backend contracts
|   |   |-- radeon_2d_backend.cpp   Dispatcher table over Blit2dEngine on SDMA1
|   |   |-- gfx11_isa.hpp/.cpp      Checked GFX11 instruction encoder (LLVM MC vectors)
|   |   |-- raster2d_shaders.hpp/.cpp Compute kernels: ROP3/4, alpha, gradients, lines
|   |   |-- raster2d.hpp/.cpp       Compute raster validation, args and PM4 dispatch
|   |   |-- vram_reservations.hpp   Linux VRAM reservation map contracts
|   |   |-- vram_reservations.cpp   VBIOS usage, top-of-VRAM, training, stolen VGA
|   |   |-- vram_allocator.hpp      Whole-VRAM buddy allocator contracts
|   |   |-- vram_allocator.cpp      drm_buddy/amdgpu_vram_mgr port
|   |   |-- vram_backend.hpp        Manager backend over allocator pools
|   |   |-- vram_backend.cpp        CPU-visible / GPU-only pool carving
|   |   |-- asic_init.hpp           Ordered bring-up contracts and report
|   |   `-- asic_init.cpp           Linux-ordered ASIC initialization sequence
|   |-- memory/
|   |   |-- manager.hpp             Allocation/backend/lease contracts
|   |   |-- manager.cpp             GPUVA reservations and quarantine
|   |   |-- page_table.hpp          DMA-page/PTE contracts
|   |   |-- page_table.cpp          Checked GFX11 system leaf PTE encoding
|   |   |-- gfxhub.hpp              Discovery and VMID0 ownership contracts
|   |   `-- gfxhub.cpp              Discovery validation/GART/TLB protocol
|   |-- acceleration/
|   |   |-- sdma.hpp                GPU-range and checked byte-writer contracts
|   |   |-- sdma.cpp                SDMA6 copy/fill/fence packets
|   |   |-- ring.hpp                Owned queue/backend contracts
|   |   |-- ring.cpp                Publication, real fences and recovery
|   |   |-- interrupt.hpp           IH6 event/routing contract
|   |   `-- interrupt.cpp           Bounded event decode and deferred fence polling
|   |-- display/
|   |   |-- modes.hpp               Timing, scanout and mode transaction contracts
|   |   |-- modes.cpp               Checked timing/scanout and confirmed modes
|   |   |-- preos_scanout.hpp       Firmware DCN3.2 scanout snapshot contracts
|   |   |-- preos_scanout.cpp       Read-only HUBP/OTG capture and adoption checks
|   |   |-- vbe.hpp/.cpp            VBE 2.0/3.0 parsing, mode choice, INT 10h sequence
|   |   |-- dcn32_cursor.hpp/.cpp   DCN3.2 HUBP/DPP cursor, placement and VUPDATE lock
|   |   |-- dcn32_otg.hpp           Disabled-OTG MMIO ownership contract
|   |   |-- dcn32_otg.cpp           Checked timing RMW/readback/quarantine
|   |   |-- dcn32_hpd.hpp           Read-only HPD sense and bounded DCN3.2 access
|   |   |-- dcn32_hpd.cpp           Verified pin and segment2 HPD sense decode
|   |   |-- dcn32_clocks.hpp/.cpp   SMU13 DALSMC mailbox, clock table, DCCG DTOs/dividers
|   |   `-- dcn32_dmub.hpp/.cpp     DMCUB regions, start, boot status and inbox1 ring
|   |-- output/
|   |   |-- sink.hpp                EDID candidate/link contracts
|   |   |-- sink.cpp                EDID decoding and RGB8 output validation
|   |   |-- dp_link.hpp/.cpp         Bounded SST AUX/DPCD/EDID discovery policy
|   |   |-- dcn32_aux.hpp/.cpp       Bounded DCN3.2 AUX SW engine under owner proof
|   |   |-- dp_training.hpp/.cpp     Bounded 8b/10b SST training over owned callbacks
|   |   |-- atom_connectors.hpp     Board connector/encoder route contracts
|   |   |-- atom_connectors.cpp     Checked ATOM tables, HPD/DDC/GPIO lookup
|   |   |-- dcn32_route.hpp/.cpp    ATOM-to-UNIPHY/DIG/AUX/HPD route proof, DDC pad mode
|   |   |-- dcn32_link_encoder.hpp/.cpp DIO encoder and PHY over VBIOS transmitter/PLL tables
|   |   |-- hdmi_scdc.hpp/.cpp      DC_I2C over DDC and HDMI 2.0 SCDC TMDS configuration
|   |   `-- display_link.hpp/.cpp   Link-side owner: route leases, clocks, PHY, DMCUB, proofs
|   |-- opengl/
|   |   |-- fixed_function.hpp      Internal bounded GL-style frontend contracts
|   |   |-- fixed_function.cpp      Lists, transforms, lighting and triangle assembly
|   |   |-- gl_api.hpp              OpenGL 1.1 scalar types and enumerant values
|   |   |-- gl_pipeline.hpp/.cpp    Frontend/backend contract, matrices, textures
|   |   |-- gl_clip.hpp/.cpp        Clip-space polygon/line clipping with attributes
|   |   |-- gl_raster.hpp/.cpp      Software reference rasterizer (BGRA8 Backend)
|   |   |-- gl_context.hpp/.cpp     OpenGL 1.1 state machine, transform, lighting
|   |   |-- gl_shader_gfx11.hpp/.cpp GFX11 fixed-function VS/PS generator (gfx11_isa)
|   |   |-- gl_gfx11_abi.hpp        Versioned shader ABI: ShaderKey, SGPR slots, NGG/PS state
|   |   |-- gfx11_registers.hpp     224 GC11 registers/989 fields, header-validated
|   |   |-- gfx11_state.hpp/.cpp    Allow-listed PM4 builder: preamble, surfaces, draw atoms
|   |   |-- gl_gfx11_backend.hpp/.cpp GL Backend over process graphics submissions
|   |   |-- gfx11.hpp               Owned shader/buffer PM4 contracts
|   |   |-- gfx11.cpp               Checked shader/user-data/draw packet encoding
|   |   |-- physical_submission.hpp/.cpp  Isolated trusted GFX resource/fence lease owner
|   |   |-- kernel_graphics_bridge.cpp   Unwired adapter to kernel queues/fences
|   |   |-- radeon_gdi_triangle.hpp/.cpp  Restricted GDI GradientFill triangle bridge
|   |   |-- radeon_gdi_triangle_win32.hpp/.cpp  Win32 HDC binding for the GDI bridge
|   |   `-- icd/
|   |       |-- icd_table.hpp       Exact ReactOS opengl32 ICD dispatch-table ABI
|   |       |-- icd_dispatch.cpp    336-entry GL 1.1 dispatch forwarding to Context
|   |       |-- icd.cpp             WGL Drv* entry points, GDI-presented fallback
|   |       `-- radeonglicd.spec    ICD DLL export list
|   |-- reactos/
|   |   |-- driver.cpp              Read-only NT diagnostic driver
|   |   |-- video.cpp               Experimental VideoPort miniport
|   |   |-- lifetime.hpp            Nonallocating C++ extension construction
|   |   |-- hardware.hpp            Native resource/MMIO ownership contracts
|   |   |-- hardware.cpp            BAR5 mapping, BAR0 VRAM window and bounded access
|   |   |-- asic_bringup.hpp        Opt-in native ASIC adapter contracts
|   |   |-- asic_bringup.cpp        Native MMIO/timing/VRAM/firmware for AsicInit
|   |   |-- gpu_process.hpp/.cpp   NT objects of the process service: exit notify, reaper thread, reset hooks
|   |   |-- gpu_process_core.hpp/.cpp Portable session state machine: dispatch, bounded reap, post-reset rebuild
|   |   |-- gpu_process_abi.hpp    Checked private process-buffer/queue request ABI
|   |   |-- process_memory.hpp/.cpp Authorized per-process VRAM/GTT buffer mapping and copy ownership
|   |   |-- process_submission.hpp/.cpp Driver-owned semantic SDMA IB and fence-retained buffer leases
|   |   |-- dp_port_native.hpp/.cpp Quiescent ATOM DP path lease, probe/training and quarantine policy; no AUX register backend
|   |   |-- dcn32_hpd_native.hpp/.cpp Read-only HPD via verified DMU segment2 and owned BAR5 lease
|   |   |-- dcn32_aux_native.hpp/.cpp One-shot AUX BAR5/HPD adapter bound through display_link proofs
|   |   |-- display_link_native.hpp/.cpp BAR5/BAR0 adapter of the opt-in display link owner
|   |   |-- vbios.hpp/.cpp          Card VBIOS sources in amdgpu_get_bios order
|   |   |-- pci_config.hpp/.cpp     Native PCI config, upstream bridges and link
|   |   |-- vram_window.hpp/.cpp    Bounded BAR0 VRAM views
|   |   |-- fence_native.hpp/.cpp   Fence lock, events and fallback poll timer
|   |   |-- preos_scanout_native.hpp/.cpp Adopted scanout mode and mapping IOCTLs
|   |   |-- radeon_2d_abi.hpp       Private Radeon 2D IOCTL ABI (opaque handles)
|   |   |-- radeon_2d_ioctl.hpp/.cpp Miniport 2D dispatcher and VideoPort glue
|   |   |-- radeon_display.hpp/.cpp radeondisp physical GDI display DLL
|   |   |-- radeon_line_identity.hpp Fixed radeondisp identity for the paired line helper
|   |   |-- radeon_opengl_info.hpp  OPENGL_GETINFO reply naming the RadeonGL ICD
|   |   |-- radeon_present.hpp      RadeonGL process escape and clipped GPU present policy
|   |   |-- process_graphics_abi.hpp Typed draw description wire format (handles only)
|   |   |-- process_graphics.hpp/.cpp Kernel-built graphics IBs, shaders and present
|   |   |-- radeon_bridge.hpp/.cpp  win32k helpers: styled-line DrvStrokePath bridge
|   |   |-- radeon_devbitmap_bridge.hpp/.cpp win32k device-bitmap creation, deletion and conversion
|   |   |-- radeon_text_engine.hpp/.cpp win32k coverage8 FreeType glyph bridge helpers
|   |   |-- radeon_text_bridge.hpp Kernel-only Radeon glyph Notify and read-only identity contract
|   |   |-- vbe_modeset.hpp/.cpp    VideoPort Int10 binding and boot-firmware probe
|   |   |-- radeon_cursor_policy.hpp Shared VIDEO_POINTER layout and validation
|   |   |-- radeon_cursor_native.hpp/.cpp Pointer IOCTLs and cursor VRAM (HardwareCursor=1)
|   |   |-- radeon_cursor_client.hpp/.cpp radeondisp DrvSetPointerShape/DrvMovePointer logic
|   |   |-- driver_blob.hpp         Reusable native file/blob ownership
|   |   |-- driver_blob.cpp         Bounded fixed-name firmware/ROM reads
|   |   |-- firmware_file.hpp       Scoped nonpaged firmware ownership
|   |   |-- firmware_file.cpp       Exact native NT file acquisition/validation
|   |   |-- dma.hpp                 Native DMA adapter/buffer/lease contracts
|   |   |-- dma.cpp                 Real VideoPort common buffers and shutdown
|   |   |-- lightning_abi.hpp        Shared fixed-width NT5 transport ABI
|   |   |-- lightning_protocol.hpp/.cpp Checked mailbox framing and capabilities
|   |   |-- lightning_exchange_policy.hpp/.cpp Renderer resource/range validation
|   |   |-- lightning_transport.hpp/.cpp Native BAR/DMA ownership and optional device-owned QPC aggregates
|   |   |-- lightning_ring.hpp       Shared-RAM request ring, seqlocked sync timeline, posted failures
|   |   |-- lightning_completion.hpp  Shared actual submission/retirement ledger
|   |   |-- lightning_renderer.hpp/.cpp Native process/resource/render ownership
|   |   |-- lightning_image_receipt.hpp Per-image present receipts, producer points and pinned reads
|   |   |-- lightning_present_request.hpp Checked version-1/2 present request decoding
|   |   |-- lightning_present_abi.hpp Checked virtual display contracts
|   |   |-- lightning_scanout.hpp/.cpp Portable mode/scanout lifetime validation
|   |   |-- framebuffer_shadow.hpp/.cpp Cached GDI surface ownership and hooks
|   |   |-- framebuffer_primitives.hpp/.cpp GPU cosmetic-line admission
|   |   |-- line_coverage.hpp/.cpp     Bounded engine-compatible integer mask
|   |   |-- stretch_mask.hpp          Engine stretch-mask draw-decision plane
|   |   |-- framebuffer_engine_sync.cpp Active-primary GdiFlush GPU barrier
|   |   |-- framebuffer_shadow_abi.hpp Read-only runtime counters and timing
|   |   |-- framebuffer_shadow_policy.hpp Checked dirty bounds and pitch
|   |   |-- lightning_video.cpp       Native virtual display mode, mapping and cursor ownership
|   |   |-- lightning_display.hpp/.cpp   Shared display/presentation client
|   |   |-- lightning_staging_resolve.hpp Generation-exact staging release resolution
|   |   |-- lightning_cursor_snapshot.hpp Owned canonical cursor packet
|   |   |-- lightning_cursor_policy.hpp Shared host/guest cursor validation
|   |   |-- framebuffer_cursor_recovery.hpp/.cpp Engine-owned failure recovery
|   |   |-- lightning_gdi_abi.hpp      Private typed native GDI IOCTLs and counters
|   |   |-- lightning_gdi_geometry.hpp Bounded clips and upload geometry
|   |   |-- lightning_gdi_lifecycle.hpp Reset proof and promotion scheduling
|   |   |-- lightning_gdi_native.hpp/.cpp Kernel GDI transfer/fence ownership
|   |   |-- lightning_gdi_client.hpp/.cpp Cached/GPU surface coherence and hooks
|   |   |-- engine_bitmap.hpp         Scoped engine-bitmap normalization
|   |   |-- brush_snapshot.hpp        Checked realized tile ownership
|   |   |-- framebuffer_stretch_guard.hpp/.cpp Paired kernel stretch orientation
|   |   |-- framebuffer_brush_origin.hpp/.cpp Checked DC origins and pattern phases
|   |   |-- framebuffer_clip_visibility.hpp/.cpp Explicit empty-region DDI semantics
|   |   |-- framebuffer_draw_escape.hpp/.cpp Captured escapes, owner-deferred DC visibility and DIB USER lifetime
|   |   |-- framebuffer_text_batch.hpp/.cpp Bounded cached/GPU glyph-batch ownership
|   |   `-- lightning_driver.cpp     NT5 PnP entry points and lifecycle
|   `-- tools/
|       |-- inspect.cpp             PCI/ROM/EDID/firmware inspection
|       |-- file_support.hpp        Shared scoped and bounded user-mode file I/O
|       |-- report.hpp              Shared PCI/BAR formatter
|       |-- windows_support.hpp     Scoped NT service/tool handles and paths
|       |-- control.cpp             Diagnostic installer and client
|       |-- radeon_install_policy.hpp/.cpp Physical install rules, firmware list, win32k scan
|       |-- radeon_installer.hpp/.cpp Journaled install/upgrade/rollback/remove/recover engine
|       |-- radeon_win32_system.hpp/.cpp Win32/SetupAPI binding of the installer seam
|       |-- radeon_control.cpp      radeonctl command-line tool
|       |-- sha256.hpp              Bounded SHA-256 for installed-file verification
|       |-- pnp_support.hpp         Bounded shared PnP identity selection
|       `-- lightning_control.cpp   NT5 transport installer and capability client
|-- patches/
|   |-- reactos-pci-resources.patch  Native safe PCI integration
|   |-- reactos-cursor-recovery.patch Opt-in checked GDI pointer recovery
|   |-- reactos-text-batch.patch     Source-pinned FreeType notifications
|   |-- reactos-stretch-guard.patch  Optional kernel mirror orientation
|   |-- reactos-radeon-bridge.patch  Opt-in radeondisp styled-line stroke bridge
|   |-- reactos-radeon-devbitmap.patch Opt-in radeondisp device bitmaps (DrvCreate/DeleteDeviceBitmap)
|   |-- reactos-radeon-text-bridge.patch Opt-in radeondisp coverage8 text bridge
|   |-- reactos-brush-origin.patch   Checked DC and generated DIB pattern phases
|   |-- reactos-clip-visibility.patch Opt-in empty visible-region classification
|   |-- reactos-draw-escape.patch    Native clipped DrawEscape syscall bridge
|   |-- reactos-pnp-query-remove.patch Opt-in Device Manager disable as query-remove/remove
|   |-- reactos-acpi-hotplug.patch   Opt-in QEMU ACPI PCI hot-unplug with veto before _EJ0
|   |-- reactos-win32k-display-release.patch Opt-in release of a removed non-primary display
|   `-- reactos-framebuffer-shadow.patch Opt-in cached framebuffer glue
|-- lightningnt5.inf               Exact Lightning PCI PnP binding
|-- radeondisplay.inf              Physical Navi31 display binding (opt-ins default 0)
|-- third_party/qemu_lightning/    Unmodified shared transport header and COPYING
|-- tests/
|   |-- common_tests.cpp            Binary/PCI/BIOS boundary checks
|   |-- acceleration_tests.cpp      Packet/queue/completion/recovery checks
|   |-- memory_tests.cpp            Allocation/PTE/discovery/GART/TLB checks
|   |-- memory_test_backend.hpp     Shared test-only backing fixture
|   |-- interrupt_tests.cpp         IH decode/routing checks
|   |-- display_tests.cpp           Scanout, transactions and disabled-OTG checks
|   |-- output_tests.cpp            EDID/ATOM framing and routing checks
|   |-- firmware_tests.cpp          Container/version/part bounds and private fixtures
|   |-- discovery_tests.cpp         Shared parser version/extent/checksum checks
|   |-- discovery_test_fixture.hpp  Shared synthetic discovery fixtures
|   |-- psp_boot_tests.cpp          Boot ordering, timeouts and quarantine checks
|   |-- asic_test_device.hpp        Shared synthetic Navi31 register/VRAM model
|   |-- ucode_test_files.hpp        Synthetic GC/SMU/SDMA/DMCUB/TA headers
|   |-- vram_discovery_tests.cpp    IFWI, indirect VRAM and runtime-database checks
|   |-- nbio_tests.cpp              Discovery, strap, HDP and placement checks
|   |-- ucode_tests.cpp             Payload order, types and header bound checks
|   |-- psp_ring_tests.cpp          Ring, frame, command, fence and quarantine checks
|   |-- smu13_tests.cpp             Mailbox, PPTable, PCIe and thermal-alert checks
|   |-- atom_firmware_tests.cpp     Firmwareinfo and BoardTable parsing checks
|   |-- psp_training_tests.cpp      Training command, save/restore and fault checks
|   |-- gmc11_tests.cpp             GART layout, hub registers and TLB checks
|   |-- ih6_tests.cpp               IH ring, doorbell and quarantine checks
|   |-- gfx11_init_tests.cpp        Autoload, RS64, clear-state and CP checks
|   |-- gfx11_queues_tests.cpp      MQD images, MES frames, ring tests and faults
|   |-- mes_test_model.hpp          Shared MES/KIQ/CP firmware model
|   |-- mes11_tests.cpp             KIQ/scheduler order, frames and fault checks
|   |-- mes_selftest_tests.cpp      Self-test VM, frames, ring/IB tests and quarantine
|   |-- sdma6_init_tests.cpp        SDMA ring start, ring/IB tests and halt checks
|   |-- clock_gating_tests.cpp      Gate/ungate sequences, safe mode and GFXOFF checks
|   |-- irq_service_tests.cpp       IV decode, wrap/overflow, routing and enable checks
|   |-- vbios_rom_tests.cpp         SMUIO ROM read, image validity and VFCT checks
|   |-- psp_ta_tests.cpp            TA frames, RAS flags, boot-config and unload checks
|   |-- pci_caps_tests.cpp          Capability walks, partner link and save/restore plans
|   |-- fence_tests.cpp             Fence ordering, fallback, timeouts and faults
|   |-- gpuvm_tests.cpp             PDE/PTE values, fragments, flushes and PASIDs
|   |-- gpu_process_restart_tests.cpp Production native reset-retirement admission fixture
|   |-- gpu_process_core_tests.cpp  Exit reaping, escalation and post-reset session reconstruction
|   |-- lightning_image_receipt_tests.cpp Async present receipt decisions and settlement
|   |-- gpu_process_abi_tests.cpp   Process ABI v1-v4 encoding and outcomes
|   |-- process_memory_tests.cpp    Process buffers, GPUVA mappings, reset retirement and lost buffers
|   |-- process_submission_tests.cpp Synthetic MES/GPUVM SDMA transfer and fence-lease fixture
|   |-- vram_reservations_tests.cpp Reservation maps and arena placement
|   |-- vram_allocator_tests.cpp    Buddy allocation, placement and pool backends
|   |-- user_queue_tests.cpp        Queue MQDs, PASIDs, doorbells and MES frames
|   |-- scheduler_tests.cpp         Credits, ordering, timeouts and recovery
|   |-- preos_scanout_tests.cpp     Scanout capture, refusal and re-check
|   |-- dp_link_tests.cpp           Synthetic AUX/DPCD/EDID and HPD boundaries
|   |-- dp_training_tests.cpp       Synthetic PHY/AUX training and cleanup bounds
|   |-- dp_port_native_tests.cpp    Synthetic route/epoch/HPD lease and reset-proof boundaries
|   |-- dcn32_hpd_tests.cpp         Synthetic read-only HPD sense and aperture boundaries
|   |-- dcn32_aux_tests.cpp         Synthetic AUX arbitration, framing and quarantine
|   |-- dcn32_display_model.hpp     Shared synthetic DCN3.2/MP1 register model
|   |-- dcn32_route_tests.cpp       Route binding, quarantine and DDC pad sequences
|   |-- dcn32_clocks_tests.cpp      DALSMC, clock table, update order and DTOs
|   |-- dcn32_link_encoder_tests.cpp DIO registers and VBIOS table parameters
|   |-- hdmi_scdc_tests.cpp         DC_I2C transactions and SCDC writes
|   |-- dcn32_dmub_tests.cpp        DMCUB layout, start, ring and quarantine
|   |-- display_link_tests.cpp      Link owner leases, proofs, DP training, TMDS order
|   |-- blit2d_tests.cpp            Exact SDMA 2D packets, hazards and faults
|   |-- radeon_2d_ioctl_tests.cpp   IOCTL decode, handles, sessions and bounds
|   |-- radeon_2d_backend_tests.cpp Backend over a simulated SDMA ring
|   |-- gfx11_interp.hpp            Independent GFX11 decoder/wave32 interpreter
|   |-- gfx11_isa_tests.cpp         Encodings, rejections and interpreter checks
|   |-- raster2d_tests.cpp          Pixel-exact kernels against GDI engine references
|   |-- gdi_engine_reference.hpp    Test-only ports of ReactOS stretch/line engine code
|   |-- vbe_tests.cpp               Fake VBE BIOS sequence and refusals
|   |-- radeon_opengl_info_tests.cpp OPENGL_GETINFO escape contract
|   |-- radeon_bridge_tests.cpp     Stroke bridge identity, arguments and refusals (Wine)
|   |-- radeon_devbitmap_bridge_tests.cpp Device-bitmap bridge paths and conversions (Wine)
|   |-- radeon_devbitmap_ddi_tests.cpp Production DDI with the bridge over a win32k model (Wine)
|   |-- dcn32_cursor_tests.cpp      Cursor registers, placement and lock window
|   |-- radeon_cursor_tests.cpp     Pointer shapes, IOCTL decode and refusals
|   |-- asic_init_tests.cpp         Whole-sequence order, training gate and recovery
|   |-- atom_asic_init_rom.hpp      Synthetic ASIC_Init ROM seam for repost/recovery tests
|   |-- atom_test_builder.hpp       Programmatic ATOM image encoder (no binary fixtures)
|   |-- atom_reference_model.hpp    Independent atom.c reference model and program generator
|   |-- atom_interpreter_tests.cpp  Opcode semantics, reference cross-check and fuzzing
|   |-- atom_repost_tests.cpp       Re-POST sequence, failures and resume end to end
|   |-- opengl_tests.cpp            Frontend semantics and exact GFX11 packet checks
|   |-- lightning_protocol_tests.cpp Mailbox framing and fault checks
|   |-- lightning_exchange_policy_tests.cpp Renderer boundary checks
|   |-- lightning_transport_timing_tests.cpp Production QPC accounting and lifetime boundaries
|   |-- transport_timing_native/ntddk.h Explicit CPU-test declarations, not a native ABI
|   |-- reactos_pci_bar_tests.cpp    Decode/restore transaction fault checks
|   |-- framebuffer_shadow_tests.cpp Cached geometry and arithmetic checks
|   |-- line_coverage_tests.cpp      All octants, huge endpoints and mask bounds
|   |-- stretch_mask_tests.cpp      Masked stretch against the engine reference
|   |-- brush_snapshot_tests.cpp     Owned brush mask, MIX and signed phase
|   |-- framebuffer_guest_smoke.cpp Real GDI/cursor operations and timings
|   |-- framebuffer_resize_guest.cpp Resize repaint, delayed canvas capture and passive counters
|   |-- lightning_cursor_tests.cpp Shared cursor bounds and overlay geometry
|   |-- lightning_cursor_lifecycle_tests.cpp Production display client fixture
|   |-- lightning_staging_resolve_tests.cpp Lost-ack, duplicate, reorder and recycle cases
|   |-- lightning_lease_transfer_tests.cpp Removal transfer across lifecycle states
|   |-- lightning_engine_fixture.hpp Reusable SDK allocation/I/O fixture
|   |-- lightning_gdi_client_tests.cpp Production GDI client lifecycle
|   |-- lightning_gdi_native_query_tests.cpp Actual native query/ledger fixture
|   |-- framebuffer_text_batch_tests.cpp Production glyph notification helper
|   |-- framebuffer_stretch_guard_tests.cpp Bounded paired-call admission
|   |-- framebuffer_brush_origin_tests.cpp Current DC and phase boundary checks
|   |-- framebuffer_clip_visibility_tests.cpp Public DDI empty-region boundaries
|   |-- framebuffer_draw_escape_owner_tests.cpp Production foreign-owner and deferred-refresh gates
|   |-- framebuffer_cursor_recovery_tests.cpp Actual SDK recovery ABI fixture
|   `-- guest_smoke.cpp             Opt-in native installer/IOCTL/removal tests
`-- Binaries/
    `-- .build/                    Disposable compiler outputs
```
