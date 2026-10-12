# Shards that get the RAM fast-path inlining (MKWV_SHARD_MEMORY_INLINE). Everything else is still
# -O2 with the cheap hot-inline helpers, and compiles ~2.4x faster.
#
# Chosen from the race profiles (guest PROF self + LR samples, static62-static64 logs, game and
# sound threads), mapped to shards by address. These 26 hold ~98% of the sampled guest time;
# the leaf libraries the profiler under-counts (MTX, libm, OS, nw4r::math/ut, memcpy) are in.
# Moving a shard in or out of this list recompiles only that shard (plus a relink).
# Regenerate from HPROF/PROF logs with scripts/rank-hot-shards.py.
set(MKWV_HOT_SHARDS
  shard_2ec6bc5bc8c79acd0aa4c714.cpp  # nw4r::snd SoundThread, Voice::CalcAxMix, SeqTrack
  shard_011d0539b5dfea6db4aaa473.cpp  # nw4r::g3d ScnMdl::G3dProc, nw4r::lyt Pane/Material
  shard_4fdd2cff0fa67a3564d0f936.cpp  # nw4r::snd ChannelManager, SeqPlayer, AxVoice; nw4r::math
  shard_78f4ce970a10c48ea2ad2dea.cpp  # RaceScene::OnCalc
  shard_87efbcd29d8a826b2317077d.cpp  # nw4r::snd voice code, nw4r::ut lists/streams
  shard_61feb4c54e0218ed73672236.cpp  # EGG ScnRenderer, ScnRootEx, LightMgr, LightMap
  shard_17196ef445b07e40d0361f82.cpp  # Audio::RaceRSARPlayer, ObjectActor
  shard_19fef145a54d5a7358c67a07.cpp  # GameScene calc/draw, EGG Screen/StateGX
  shard_d92fca980928ef8eec7d0d98.cpp  # nw4r::g3d G3DState, LoadMaterial, LightSetting
  shard_07b1083f963154f17a91052a.cpp  # EGG ExpHeap, ExpParticleManager
  shard_bad1427c05373b76e7c860c9.cpp  # AX: __AXOutAiCallback, __AXSyncPBs
  shard_b2d2d8078ff03ea83efa8cc9.cpp  # ScnMgrRace::Draw, RaceCamera
  shard_c3c61faa071e10fe7d297ae8.cpp  # ScnMgr draw/update
  shard_bc33b60589d9aef4e374f28f.cpp  # OS (interrupts, messages, alarms, heap)
  shard_ed60942cd5fc653538845a05.cpp  # MTX / PSMTX, OS heap
  shard_7db4fa304cba2d91498c850c.cpp  # 0x8080B578 (race objects)
  shard_7db093e5dc9c15b6720d9a3f.cpp  # nw4r::ef particles
  shard_731fc3775c7fcd650b4075b4.cpp  # nw4r::g3d ResAnmChr (animation), nw4r::math
  shard_70bb677eec844cf4cdcf9fb0.cpp  # DVD/VD, GX callbacks
  shard_e417ba8130b41128e3b0f8a5.cpp  # WheelPhysicsHolder
  shard_d0d03fcb1fa45e8f72498874.cpp  # KCL collision
  shard_3b6c60158eb92c0dac09869a.cpp  # HUD layout drawing (TextPane/PicturePane/MainLayout)
  shard_067cb4790f9deaab9d250f40.cpp  # libc (memcpy/memset, qsort)
  shard_9cddda25e54c6977029fed5f.cpp  # Kart physics/collision, Hitbox
  shard_35b85db13ba0e5f68e43abfe.cpp  # RouteCurved, Audio::Engine3D
  shard_f818dc72cb0c0a8f7565b37f.cpp  # libm (__ieee754_*)
)
