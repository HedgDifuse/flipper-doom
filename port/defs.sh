# Engine configuration shared by the simulator, the size check and the device build.
DOOM_DEFS=(
    -DFLAT_SPAN -DC_ONLY -DFLIPPER_DOOM
    -DVIEWWINDOWWIDTH=120 -DVIEWWINDOWHEIGHT=64 -DMAPWIDTH=120
    -DMAX_CACHE_ENTRIES=16 -DPG_SLOTS=16
    '-DMAXOPENINGS=(VIEWWINDOWWIDTH*7)' -DMAXDRAWSEGS=72 -DMAXVISSPRITES=40
    -DSIGHT_MEMO_TICS=4 -DSIGHT_MEMO_IDLE_TICS=20
    # the zone: fz_zone_regions() (sim: FZ_ZONE_KB, Flipper: the loader)
    '-DWAD_FILE="FZDOOM.WAD"' -Dstricmp=strcasecmp
)
