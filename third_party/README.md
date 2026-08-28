# third_party

## libtfmxaudiodecoder

Hülsbeck / Hippel TFMX decoder by Michael Schwendt (GPLv2+).

    git clone --depth 1 https://github.com/mschwendt/libtfmxaudiodecoder \
      third_party/libtfmxaudiodecoder
    cp third_party/Config.h third_party/libtfmxaudiodecoder/src/Config.h

The shipped `Config.h` is a tiny autoconf stand-in so we do not run
`./configure`. The plugin only *claims* Hülsbeck TFMX
(`.tfx` / `.tfm` / `.mdat` / `.tfmx`); Hippel / Future Composer `.fc`
is rejected in CheckFile.

Do not commit a nested `.git` or build trees.

