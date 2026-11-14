# Generated: per-file pins for sources fetched as single files (sha256, hex).
{
  bootlinCedrus = {
    rev = "4e9497947c56f10351ed8c000605dbe47d4aee46";
    dir = "drivers/staging/media/sunxi/cedrus";
    files = {
      "Kconfig" = "24b6b47d21f7a5f389accf1670ba9c9b555976f431d2fa6526069e9246fecd07";
      "Makefile" = "149b3131d9efec20a3354d00c2edfdd6b19a6201b9948bea652f1bd07d15d24e";
      "TODO" = "1d4d6285890591e6f1810348fc2b1637525d4f59897679ec4208c7fa2c72dd43";
      "cedrus.c" = "da975b62f5c7b28ba9610afca185841af84c3cb5f096e098f847ebee01e99ae7";
      "cedrus.h" = "a5194f51510274e54ec302f089ce101f7752698e69c9f99090544bf3498ab460";
      "cedrus_context.c" = "727a763267604e4ffe03609af2bd4e884c54535525e69f82a79076aaacd5fa40";
      "cedrus_context.h" = "57c884120b4a8083f572b5ba8c2cf7d0e85b41c5a95f6aa064cedc564aa6627d";
      "cedrus_dec.c" = "82500950e0920658184f934db81c8061be6ded3b89e15bb0e82383290c89812a";
      "cedrus_dec.h" = "905a2b0455b7883225a9e953116bf6f6d1372e33cf1296310e74b54269ece5d2";
      "cedrus_dec_h264.c" = "70ab6cfaa8c46441e7cad5689f2598f3db29c400d95df3241096e52f3ef0e886";
      "cedrus_dec_h264.h" = "f0650968c247a5dc30f211d0da4985beafa8abe3323cbe9c38801acd74d4741c";
      "cedrus_dec_h265.c" = "e69e83487b86c976f4b74c7c728efb866e97db1aab2a460ddafc2bbe6df0c5be";
      "cedrus_dec_h265.h" = "4e68280103ee5825b469dda0c8ef81d411464cbf907b1b0553e3f9ddf69c90fe";
      "cedrus_dec_mpeg2.c" = "6f9ca5e3c36f5c23cc8189e425cb7a05e43d48278638bf6b5564403c9ad9f796";
      "cedrus_dec_mpeg2.h" = "99aefc0f4e46079fadee9c9d144bc417f92f979944cf9e21b7c25482519e7543";
      "cedrus_dec_vp8.c" = "a63ec377fc27c9fafdeafc0c3d61fccb2f4d545f267440b455a0c807b0e1b32b";
      "cedrus_dec_vp8.h" = "bd55a641817009b3de1f03d65a260563212be4e160bf13e923c514a5dbdcebd3";
      "cedrus_enc.c" = "13ff1e82c0f8047af3b82b997fc1fbefec9caada464f25c25648f37c33e16699";
      "cedrus_enc.h" = "696f8734a897e8e65a674052a22e2e0797a35f3d8a581e2b4a1bb36e6ce44469";
      "cedrus_enc_h264.c" = "e7ce56670af127a167eeaa3ca8e731e42e87670ce92647b3c6e320813ad3e1df";
      "cedrus_enc_h264.h" = "491a56ad3b97fdc0848d7df6e2643c1f9e7f52be1d4d65d929dd33e74b7c19f0";
      "cedrus_engine.c" = "4cc57ac62387770f576c56227b2e608f5b66a036b149366fac4c84a975b3ca15";
      "cedrus_engine.h" = "c248946cec0342dcd5dd5c31ac0a22f522549fc96f0d5aa974474b9cf1721399";
      "cedrus_proc.c" = "3b842e9832d74607948e64ad34969fbc91023281e5d94d3606ff08e2d60d0340";
      "cedrus_proc.h" = "a927199968c423b15befafd83ec210db4815bfe0040356e127439ddef25fc2ab";
      "cedrus_regs.h" = "10e8c48be7e179c66a2b339bb2e6142e4dfc5c12b128b79b0afad008c8cd63b3";
    };
  };
  nvdlaFirmware = {
    rev = "79538ba1b52b040a4a4645f630e457fa01839e90";
    files = {
      "bdma.c" = { path = "kmd/firmware/bdma.c"; sha256 = "1fa979b144512307d2d5a4acf80fd67defb54554ab5ba7dd911b77871aaf42fc"; };
      "cache.c" = { path = "kmd/firmware/cache.c"; sha256 = "9c7b6a5414102db8a4140d8e52d61756db6c779b370624cc6a83734394d71577"; };
      "cdp.c" = { path = "kmd/firmware/cdp.c"; sha256 = "28e796f9d9c0003e533e07a0a38ce7ba6153851fd7bbc785868fe392b9801a7a"; };
      "common.c" = { path = "kmd/firmware/common.c"; sha256 = "bc819523f5cf89b2aef850c0dc3a671d519cef1889333ccc3f1f9fe588371fbc"; };
      "common.h" = { path = "kmd/firmware/common.h"; sha256 = "8fea6c2c244b7bb272b22560020644535e19fe7b61d75091e152acaf637e3ee9"; };
      "conv.c" = { path = "kmd/firmware/conv.c"; sha256 = "a651a0ded8dbcfd7f2d1ffa2da20c908aa163a82ddfcbaca077cbedd1e9f7bb3"; };
      "dla_engine_internal.h" = { path = "kmd/firmware/dla_engine_internal.h"; sha256 = "d160727b9df76af59833c858b1a5f9f06a47800cf5d62386d8184b69944f5708"; };
      "engine.c" = { path = "kmd/firmware/engine.c"; sha256 = "e96a043f83413d194622e55b3a13e47c54b675061c66fc7bf47d6ae57dc14c75"; };
      "engine_data.c" = { path = "kmd/firmware/engine_data.c"; sha256 = "07b81e64f2fabd112668938d6d6a424e94a2dda0c701947550ceb2e0ea5f77bf"; };
      "engine_debug.c" = { path = "kmd/firmware/engine_debug.c"; sha256 = "0bdcea3760a466bca658781789adb841753dda744fede8838f620aa12d50d64f"; };
      "engine_debug.h" = { path = "kmd/firmware/engine_debug.h"; sha256 = "73df6e108bfeceb11f4c638da3619ffe26a0aec3411cc9b1c87fa584b1d0d713"; };
      "engine_isr.c" = { path = "kmd/firmware/engine_isr.c"; sha256 = "c958256c9dd990cb4dcd1b392c5f15e969265a333137bc9fb081b84039f7fa30"; };
      "include/dla_debug.h" = { path = "kmd/firmware/include/dla_debug.h"; sha256 = "7b4c2f7cbce2aa44383f2cca88f42ccd7f54a9cfbe1b1e521b3358a002a183d3"; };
      "include/dla_engine.h" = { path = "kmd/firmware/include/dla_engine.h"; sha256 = "53c62d20f19b3861f68b59132855d78eeb66b6ca70bbc598033e47de2b4e5ebc"; };
      "include/dla_err.h" = { path = "kmd/firmware/include/dla_err.h"; sha256 = "11f1698461815d103f79753d475ff2b5f72fa2342f405bbbd14e378a4fead5e0"; };
      "include/dla_fw_version.h" = { path = "kmd/firmware/include/dla_fw_version.h"; sha256 = "e794c297fd2b51f449ced52d85219f1933919a6dfe9911e9a49a7fcfbd023cb5"; };
      "include/dla_interface.h" = { path = "kmd/firmware/include/dla_interface.h"; sha256 = "226431e91454049656fe21bc4dd9212edf87f091fae568e2c619fdd1c641cd13"; };
      "include/dla_sched.h" = { path = "kmd/firmware/include/dla_sched.h"; sha256 = "b826be800e6c5d4ba55a1d0a2ec36c0a708fa150e9bf28aeb8ba579e18ab4e61"; };
      "include/nvdla_interface.h" = { path = "kmd/include/nvdla_interface.h"; sha256 = "3d220da0707392e0494d5b95a9e44faa0bd094086fc8e9d8839f57ee5e8330e5"; };
      "include/opendla.h" = { path = "kmd/firmware/include/opendla.h"; sha256 = "64a0463225064aa7311936a7235c37cbfdb7f5eb214dbdfb125fed299de71ecc"; };
      "include/opendla_small.h" = { path = "kmd/firmware/include/opendla_small.h"; sha256 = "15aa2b37f35b559ff53891a58f2473a621411b03c57a3f8fbc43a862bf4745b3"; };
      "pdp.c" = { path = "kmd/firmware/pdp.c"; sha256 = "3c6ae1330acc9f874ca5923a1c6829850e92829dc0b42946001e41c62f6985d0"; };
      "rubik.c" = { path = "kmd/firmware/rubik.c"; sha256 = "7047cc0ec2fcedc2b3f2af78c8c823f58bdcbd2e3a76aa332ff2d1ae88475214"; };
      "scheduler.c" = { path = "kmd/firmware/scheduler.c"; sha256 = "6a0a1c127640dd7a678b493502f46e17f5c09f9184a287cd363999264515f0e3"; };
      "sdp.c" = { path = "kmd/firmware/sdp.c"; sha256 = "cf6c03f6869e8c2468006d246f0ea4eb94c5861aab0a5f82499ea638a48ea4e7"; };
      "LICENSE" = { path = "LICENSE"; sha256 = "7461d95f5c71170b583cd79981c2cd2b96e39473166821467c3f4595509fe5d4"; };
    };
  };
}
