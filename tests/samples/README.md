# Test samples

Place Hülsbeck TFMX pairs here for host tests. These rips are copyrighted
and must **not** be committed or uploaded.

    tests/samples/user-song.tfx + user-song.sam   # one ~42s stream (1 slot)
    tests/samples/user-mod.tfx  + user-mod.sam    # one ~6:46 stream (8 slots chained)

`/usr/bin/make test` requires `user-song.tfx` + sibling `.sam`.
`user-mod.*` is used when present. Both are exposed as **one** playlist item.
    tests/samples/kanzle/kanzle-a.tfx + kanzle-b.tfx + Set.sam
        # two TFMX Professional songs sharing ONE sample file (no kanzle-a.sam)

`kanzle/` is used when present. Opening either `.tfx` must find `Set.sam`
in that folder (not `song.sam` / `smpl.set`). Never commit or upload these.

    tests/samples/longnoise/long.tfx + long.sam   # local only: 3:46 music then junk
    tests/samples/tiny/tiny.tfx + tiny.sam        # local only: lone sample-loop, 10 min cap

`longnoise/` and `tiny/` are used when present. Never commit or upload these.
