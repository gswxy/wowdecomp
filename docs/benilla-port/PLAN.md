# benilla-bourbon → Wow64/Wow32 port queue (user: "implement that short list", 2026-10-26)

Source: /root/wotlk-renderers/benilla-bourbon (Rust/Bevy/WGSL; FORK.md explains each feature). Target: /root/wowdecomp D3D11 backend (Wow64) — Wow32 must still build; features may be D3D11-only behind CVars.
Rules: port faithfully first (copy structure + constants 1:1, translate WGSL→HLSL); every feature behind its own CVar, default ON only if cheap and safe; dither is ALLOWED (user 2026-10-26: "dither is ok btw"); no terrain parallax; no frame generation; far terrain uses real ground textures, never minimap; bakes read files via SFile (players swap assets via their own MPQs).
Per item: implement → build both exes (build-capped.sh, /run/tswow-deploy.lock) → publish through patcher immediately (/root/tswow-launcher/tools/build-release.js; check /srv/tswow-patches/manifest.json md5s) → commit locally (new commit per item is fine; never push) → tick the item in STATUS below with version string + CVars + what to check in game. Never restart the realm; never run the client/rig.

## Queue (in order)
1. Auto-exposure — crates/benilla-world/src/shaders/modern/meter.wgsl, src/modern/meter.rs (64x36 log-lum meter, eased). Into D3D11 Lighting/Lighting.hlsl post next to hdrTonemap/hdrExposure.
2. SSILVB replacing lightingSsao — crates/benilla-world/src/ssilvb.rs, src/shaders/ssilvb.wgsl. Fixed per-pixel rotation + spatial blur, no dither.
3. Emissive-derived lights → LegionLights.cpp — crates/benilla-world/src/modern/lights/emissive.rs; our research /root/wxl-light/docs/research/10-emissive-lights.md.
4. Sky-occlusion map — crates/benilla-world/src/modern/shadow.rs (sky map 0.5 yd texels); reuse TerrainSunShadow.cpp land heightfield + WMO/doodad tops.
5. Rain wetness + puddle reflections — crates/benilla-world/src/weather/wetness.rs, weather/wet_ssr.rs, src/shaders/wet_ssr.wgsl, crates/benilla-assets/src/shaders/wet_hook.wgsl.
6. Animation LOD — crates/benilla-world/src/rig_anim/pose.rs (distant rigs re-posed every 2nd-4th frame).
7. Far view — benilla WDL horizon to 10 km + seam dissolve + long-range height fog (crates/benilla-world/src/wdl.rs, view.rs, exterior_cull.rs, terrain_stream.rs, crates/benilla-assets/src/shaders/wdl.wgsl, modern/look.rs, modern/volumetrics/media.rs). KEEP our FarTrees impostors drawn over it and our FarLand real-ground colour. See memory project_benilla_farview_port.md.
8. Light grid + local cube shadows — src/shaders/modern/light_bin.wgsl, locallight.wgsl (LegionLights → binned forward lights, lift 8-light cap).
9. Probe GI — crates/benilla-world/src/modern/gi/mod.rs, src/shaders/modern/gi_trace.wgsl, probegi.wgsl.

## STATUS (agents append here)
- [x] 1 Auto-exposure — v2026.10.26-1135-autoexp (Wow64 c9b54c56…, Wow32 e0de5f69…). D3D11 only, needs `hdr 1` (it lives in the tonemap pass). CVars: `autoExposure` 1 (0 = fixed exposure), `autoExposureTarget` 0.06, `autoExposureMax` 2.25. MeterPs (Lighting.hlsl) = meter.wgsl 1:1 (64x36 R32F, 4x4 bilinear taps, log2 lum, floor 1e-4) over the world viewport; one staging readback in flight, mapped DO_NOT_WAIT; lift = clamp((target/(key*hdrExposure))^0.65, 1, max), eased tau 1.5 s, first reading taken whole, multiplies the exposure of bloom and tonemap. Deviation: no "authored light rows" fallback before the first reading / when the meter goes stale (>1 s) — the lift just holds. Vulkan untouched. In game (hdr 1): a night or a dark cave brightens over ~1.5 s, an open day is unchanged; toggle `autoExposure 0` to compare.
