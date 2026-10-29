# The font files of the font bundles (pr/font_bundle.h), each fetched by FetchContent and checked against its hash.
#
#   pr_font_bundle_fetch(<directory> <bundle>...)
#
# makes the files of the bundles named, and of the fallback every bundle installs, available in <directory>, where
# prFontBundleInstall finds them, each linked there under its name upstream. The bundles are Noto and Standard14.

include(FetchContent)

# Noto's site, at the commit the fonts are pinned to.
set(_pr_noto https://raw.githubusercontent.com/notofonts/notofonts.github.io/b966e2e80feb783346a7d3b7c14c8d6e7e1a757b/fonts)
# Noto's CJK repository, at the tag of each release.
set(_pr_noto_cjk https://raw.githubusercontent.com/notofonts/noto-cjk)

# The fallback: Noto for every script UAX #31's Table 5 lists as in modern use, with Mongolian, math, symbols and emoji,
# in sans and serif where Noto has both, from Noto's site and its CJK releases; and the Unicode Consortium's Last
# Resort, which draws the missing glyph of a code point no face serves.
FetchContent_Declare(font_NotoSans
    URL ${_pr_noto}/NotoSans/unhinted/variable-ttf/NotoSans%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=511cc04b442ffcc3b24d65c2cc780bdffaf482b20ba0aebdc8f27b37017ff0f5
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSans[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansItalic
    URL ${_pr_noto}/NotoSans/unhinted/variable-ttf/NotoSans-Italic%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=0c9c7620381d710ac16269b1f1756f9ee05fa4c3bf7293ff04ef6723fcddbe9f
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSans-Italic[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansMono
    URL ${_pr_noto}/NotoSansMono/unhinted/variable/NotoSansMono%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=8b8b934f600dbd1da306eb4018e7406a4510cccd011a686c16680bcaeacb6617
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansMono[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerif
    URL ${_pr_noto}/NotoSerif/unhinted/variable-ttf/NotoSerif%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=8918809882ffe61eb32baf3f4b3420fda6c022f2219b1b0272755d70ecabc7de
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerif[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifItalic
    URL ${_pr_noto}/NotoSerif/unhinted/variable-ttf/NotoSerif-Italic%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=e5c3ade5b59b3af8c48c24915580cab99f083bd728a81c343741fc09a0c76c9f
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerif-Italic[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansMathRegular
    URL ${_pr_noto}/NotoSansMath/unhinted/ttf/NotoSansMath-Regular.ttf
    URL_HASH SHA256=b127e84699212b6b2ef50aff58e0ebebeec04ffe6db1b9eb9e209c8c3d97b4aa
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansMath-Regular.ttf)
FetchContent_Declare(font_NotoSansSymbols
    URL ${_pr_noto}/NotoSansSymbols/unhinted/variable-ttf/NotoSansSymbols%5Bwght%5D.ttf
    URL_HASH SHA256=9dc0ef3c6eea7436e064409ae9d0e3cc1928e21ec5c1baf43096cfad208b2d0a
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansSymbols[wght].ttf)
FetchContent_Declare(font_NotoSansSymbols2Regular
    URL ${_pr_noto}/NotoSansSymbols2/unhinted/ttf/NotoSansSymbols2-Regular.ttf
    URL_HASH SHA256=c4a0a80f0041ce4be81e2478faad22776d23edb98ae3f0d19bd37044820ecf9d
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansSymbols2-Regular.ttf)
FetchContent_Declare(font_NotoSansArabic
    URL ${_pr_noto}/NotoSansArabic/unhinted/variable-ttf/NotoSansArabic%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=3b735d0b7f04ddd5c6a3d7fafba8d8216420d3cbbba93ded80f34ba37369de00
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansArabic[wdth,wght].ttf)
FetchContent_Declare(font_NotoNaskhArabic
    URL ${_pr_noto}/NotoNaskhArabic/unhinted/variable-ttf/NotoNaskhArabic%5Bwght%5D.ttf
    URL_HASH SHA256=7b81922a002a54457b75d6df7199fc6fda50a138a98c37b707b81422a9fe8bc1
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoNaskhArabic[wght].ttf)
FetchContent_Declare(font_NotoNastaliqUrdu
    URL ${_pr_noto}/NotoNastaliqUrdu/unhinted/variable-ttf/NotoNastaliqUrdu%5Bwght%5D.ttf
    URL_HASH SHA256=e7b41654dc71e75a8a96f2a6c2237ea9271aa6529cba5bdd9920073fc8dc9d21
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoNastaliqUrdu[wght].ttf)
FetchContent_Declare(font_NotoSansHebrew
    URL ${_pr_noto}/NotoSansHebrew/unhinted/variable-ttf/NotoSansHebrew%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=2a0af06d3e959ebc3f50a8c0042c24b671b6b3283d1ebbf3e7d39ef2b4ce62ba
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansHebrew[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifHebrew
    URL ${_pr_noto}/NotoSerifHebrew/unhinted/variable/NotoSerifHebrew%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=3120dec891412498c3a81392882845d1ee2df1ef16b35ca5153696625123a2d1
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifHebrew[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansDevanagari
    URL ${_pr_noto}/NotoSansDevanagari/unhinted/variable-ttf/NotoSansDevanagari%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=9f82923465ef82fb86c6b99cbdd4947ba832fe3335948de815b18adfd75947c7
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansDevanagari[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifDevanagari
    URL ${_pr_noto}/NotoSerifDevanagari/unhinted/variable-ttf/NotoSerifDevanagari%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=bc80ac535c9a7000611d004a060a15af0aa91689a69009fd943f720be4df2586
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifDevanagari[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansBengali
    URL ${_pr_noto}/NotoSansBengali/unhinted/variable-ttf/NotoSansBengali%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=f3d4a254574c1b93c2c98e2010818337e99c6a1351004e6da80a1e6cb841e8e2
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansBengali[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifBengali
    URL ${_pr_noto}/NotoSerifBengali/unhinted/variable-ttf/NotoSerifBengali%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=2af2d3d0d11295b430b03f39f538cf033c9e7136e907bdc77e67a6d82e10a787
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifBengali[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansTamil
    URL ${_pr_noto}/NotoSansTamil/unhinted/variable-ttf/NotoSansTamil%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=1b29b0df004da1349788834621642324210cd7eec03ab060ce28babdc5f395e7
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansTamil[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifTamil
    URL ${_pr_noto}/NotoSerifTamil/unhinted/variable-ttf/NotoSerifTamil%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=b5056231fea6b7c027ea6ff7a3e572c246bd8d86b1a19eb8569a41d9b5545ebb
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifTamil[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifTamilItalic
    URL ${_pr_noto}/NotoSerifTamil/unhinted/variable-ttf/NotoSerifTamil-Italic%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=07d762d6ce699fd415492c9e8da60ee0bc1b9cff4f2acfd3cbf1966ca6e7bd59
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifTamil-Italic[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansThai
    URL ${_pr_noto}/NotoSansThai/unhinted/variable/NotoSansThai%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=d5284842bca4d7af8c8fa18b6f45b9f603bb14dccc224e4eedf41f51c1867f32
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansThai[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifThai
    URL ${_pr_noto}/NotoSerifThai/unhinted/variable/NotoSerifThai%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=c52711f0aa72a87ddf720700f79799f58ccf95df49b73bbe73f1bc0b193a2c9c
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifThai[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansMyanmar
    URL ${_pr_noto}/NotoSansMyanmar/unhinted/variable-ttf/NotoSansMyanmar%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=e343a0b4a348095f666ed54007a1bbde3a6d593ff6b2e3340d18c02766be7735
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansMyanmar[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifMyanmar
    URL ${_pr_noto}/NotoSerifMyanmar/unhinted/variable-ttf/NotoSerifMyanmar%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=39e70183e8c149467f307a2c08de292b0236c2db9ab15920ac440e3b4d6733fe
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifMyanmar[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansKhmer
    URL ${_pr_noto}/NotoSansKhmer/unhinted/variable/NotoSansKhmer%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=a3bd51ea30792461164c2c6fc93c26e6f80f90b58c9e54071efcccad65cb591d
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansKhmer[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifKhmer
    URL ${_pr_noto}/NotoSerifKhmer/unhinted/variable/NotoSerifKhmer%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=9d5b4922f373d40ac057bfd3a39d9eea632a7871d3fe94301fec9d2412d7a199
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifKhmer[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansMalayalam
    URL ${_pr_noto}/NotoSansMalayalam/unhinted/variable-ttf/NotoSansMalayalam%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=021d71bd2437f79bdf7bcc8c6d4beec447387993fe0ae545360f79a8afe6fe4b
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansMalayalam[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifMalayalam
    URL ${_pr_noto}/NotoSerifMalayalam/unhinted/variable-ttf/NotoSerifMalayalam%5Bwght%5D.ttf
    URL_HASH SHA256=714dd7751d56f8f3032ea489386df4cb2fd1e71b11046424f92dc42d34e4f466
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifMalayalam[wght].ttf)
FetchContent_Declare(font_NotoSansKannada
    URL ${_pr_noto}/NotoSansKannada/unhinted/variable-ttf/NotoSansKannada%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=9addc0d54716112ac85e11bf4f3ce5b167cb91c1337c152d854e878393b5662c
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansKannada[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifKannada
    URL ${_pr_noto}/NotoSerifKannada/unhinted/variable/NotoSerifKannada%5Bwght%5D.ttf
    URL_HASH SHA256=f7bcc9b755cf6212bb35841ff0179598df8f12511e11da761fcd66868decfa26
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifKannada[wght].ttf)
FetchContent_Declare(font_NotoSansArmenian
    URL ${_pr_noto}/NotoSansArmenian/unhinted/variable/NotoSansArmenian%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=d3ae1e6d641e5b4edd803a08af1bf1a54faaf15f55e1bc09751e5d02e96106e7
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansArmenian[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifArmenian
    URL ${_pr_noto}/NotoSerifArmenian/unhinted/variable/NotoSerifArmenian%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=cbfcb332075248461fe859e83459fb4902c5a18250b4ebbbb1abca1f25258663
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifArmenian[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansGeorgian
    URL ${_pr_noto}/NotoSansGeorgian/unhinted/variable/NotoSansGeorgian%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=361376122a9c4ed8a4e9b797eae11cf2c851c8f708880d879d91e1e23fd07bab
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansGeorgian[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifGeorgian
    URL ${_pr_noto}/NotoSerifGeorgian/unhinted/variable/NotoSerifGeorgian%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=cf96f2cfc4cf98d4a4636ff64a2b65446c74a2895cff9dd5b02805e5e8f389ba
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifGeorgian[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansGujarati
    URL ${_pr_noto}/NotoSansGujarati/unhinted/variable/NotoSansGujarati%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=6dc047f62d11a3593e52093ac4abe3be94bfe579fb10f343f37c4fffc3a576b9
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansGujarati[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifGujarati
    URL ${_pr_noto}/NotoSerifGujarati/unhinted/variable/NotoSerifGujarati%5Bwght%5D.ttf
    URL_HASH SHA256=f6b42079a4bd8bf26a9a44409bb4d802a5d55f8cc094eec9491bfadb05385c11
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifGujarati[wght].ttf)
FetchContent_Declare(font_NotoSansGurmukhi
    URL ${_pr_noto}/NotoSansGurmukhi/unhinted/variable-ttf/NotoSansGurmukhi%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=7ed72759b71a49ff0179576a11d05914a378ee069b5f136a9c1cc9316ed7f7b6
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansGurmukhi[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifGurmukhi
    URL ${_pr_noto}/NotoSerifGurmukhi/unhinted/variable-ttf/NotoSerifGurmukhi%5Bwght%5D.ttf
    URL_HASH SHA256=7af90dbc3ddb3b37b863e5b61d9943af23d80c2a7c0cd604cfc0b4ccce281c56
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifGurmukhi[wght].ttf)
FetchContent_Declare(font_NotoSansLao
    URL ${_pr_noto}/NotoSansLao/unhinted/variable-ttf/NotoSansLao%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=7b622412ef9ec6f58dd7ef197dc0e3259c8efb703090a637e4da38a69cfd8af3
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansLao[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifLao
    URL ${_pr_noto}/NotoSerifLao/unhinted/variable-ttf/NotoSerifLao%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=d296f5a2945a8a8417ec3704bea7b0bc7752758bbbcfdde5a3eec34e4f53da4d
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifLao[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansMongolianRegular
    URL ${_pr_noto}/NotoSansMongolian/unhinted/ttf/NotoSansMongolian-Regular.ttf
    URL_HASH SHA256=061c4b40b27f4cd391441262f6d35a8959052300f0495da823204024d8331de4
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansMongolian-Regular.ttf)
FetchContent_Declare(font_NotoSansOriya
    URL ${_pr_noto}/NotoSansOriya/unhinted/variable-ttf/NotoSansOriya%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=f347ec8e38979b89718c0b673e7fc532b98bbac6e7d958fc18256022c2090956
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansOriya[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifOriya
    URL ${_pr_noto}/NotoSerifOriya/unhinted/variable-ttf/NotoSerifOriya%5Bwght%5D.ttf
    URL_HASH SHA256=2f508ec4332045436b2b04904a2c2505357535458d881438ac9089336b78cd11
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifOriya[wght].ttf)
FetchContent_Declare(font_NotoSansTelugu
    URL ${_pr_noto}/NotoSansTelugu/unhinted/variable/NotoSansTelugu%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=d34981c6c3bbef078cab333a1f9b6e984daa6ad2f2e4b068ebbfea1c31602b35
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansTelugu[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifTelugu
    URL ${_pr_noto}/NotoSerifTelugu/unhinted/variable/NotoSerifTelugu%5Bwght%5D.ttf
    URL_HASH SHA256=d432b5136b5dc45e5cb51a665fb523b1a01a32319b1df0af9c6062b19a554df3
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifTelugu[wght].ttf)
FetchContent_Declare(font_NotoSerifTibetan
    URL ${_pr_noto}/NotoSerifTibetan/unhinted/variable-ttf/NotoSerifTibetan%5Bwght%5D.ttf
    URL_HASH SHA256=91f22dcbb8f53430bcae1ef994bf2ef892e4ebecefa7b3869fd6fc2bbb6aa7d3
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifTibetan[wght].ttf)
FetchContent_Declare(font_NotoSansEthiopic
    URL ${_pr_noto}/NotoSansEthiopic/unhinted/variable-ttf/NotoSansEthiopic%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=1f79abca0ecbd8d0746cd9a5493ba6cbcc7f49ce01f3f4a44f9f482c6cc9421a
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansEthiopic[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifEthiopic
    URL ${_pr_noto}/NotoSerifEthiopic/unhinted/variable-ttf/NotoSerifEthiopic%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=4efc586999280139fa395739c2b200a0b0cb83f1e70c7665747aa089b019b3f5
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifEthiopic[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansSinhala
    URL ${_pr_noto}/NotoSansSinhala/unhinted/variable-ttf/NotoSansSinhala%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=1fa6c155406689439bbe060b55ec085f25da7f0b0604e59681ae41b9bb9fedd4
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansSinhala[wdth,wght].ttf)
FetchContent_Declare(font_NotoSerifSinhala
    URL ${_pr_noto}/NotoSerifSinhala/unhinted/variable-ttf/NotoSerifSinhala%5Bwdth%2Cwght%5D.ttf
    URL_HASH SHA256=bc569746b2ab71e85861bd511f9b627569f520847401f52a5b3d937ec6548709
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifSinhala[wdth,wght].ttf)
FetchContent_Declare(font_NotoSansThaana
    URL ${_pr_noto}/NotoSansThaana/unhinted/variable/NotoSansThaana%5Bwght%5D.ttf
    URL_HASH SHA256=7f7cfa551b699d97fd1d7b469514d99ad3790d45a56f5bfce24b9956a074ad97
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansThaana[wght].ttf)
FetchContent_Declare(font_NotoSansSCVF
    URL ${_pr_noto_cjk}/Sans2.004/Sans/Variable/OTF/Subset/NotoSansSC-VF.otf
    URL_HASH SHA256=d13ed01ec8aa45d6178999b648e96fb92150683e9f8e2a581f2acf208dcbe44b
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansSC-VF.otf)
FetchContent_Declare(font_NotoSansTCVF
    URL ${_pr_noto_cjk}/Sans2.004/Sans/Variable/OTF/Subset/NotoSansTC-VF.otf
    URL_HASH SHA256=a080726eee9d78ec74fd95184a79c58c3085f10713e26876aab0130f543e9120
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansTC-VF.otf)
FetchContent_Declare(font_NotoSansHKVF
    URL ${_pr_noto_cjk}/Sans2.004/Sans/Variable/OTF/Subset/NotoSansHK-VF.otf
    URL_HASH SHA256=3432530bc55e0943f878a3b18bb062e7954654c04e9d66c2ae80b6eb62d176c6
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansHK-VF.otf)
FetchContent_Declare(font_NotoSansJPVF
    URL ${_pr_noto_cjk}/Sans2.004/Sans/Variable/OTF/Subset/NotoSansJP-VF.otf
    URL_HASH SHA256=85e5ef353081175fb9f764f037c550dd4b5ad913cb030c0de98a5d4d4018014b
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansJP-VF.otf)
FetchContent_Declare(font_NotoSansKRVF
    URL ${_pr_noto_cjk}/Sans2.004/Sans/Variable/OTF/Subset/NotoSansKR-VF.otf
    URL_HASH SHA256=e647f53b18a4823647a51bcbdac866617701a4dd8ce495bac2db5ecea88d8f21
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSansKR-VF.otf)
FetchContent_Declare(font_NotoSerifSCVF
    URL ${_pr_noto_cjk}/Serif2.003/Serif/Variable/OTF/Subset/NotoSerifSC-VF.otf
    URL_HASH SHA256=71b4d3ded2d90ff43bb75a4e48cdbe170f0b8d5486dc89ff87f2a1728b56da64
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifSC-VF.otf)
FetchContent_Declare(font_NotoSerifTCVF
    URL ${_pr_noto_cjk}/Serif2.003/Serif/Variable/OTF/Subset/NotoSerifTC-VF.otf
    URL_HASH SHA256=20a6ab96b13b433e87019ad0cdf72aa522c0db5d3138acab00d076ccf18e63ef
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifTC-VF.otf)
FetchContent_Declare(font_NotoSerifHKVF
    URL ${_pr_noto_cjk}/Serif2.003/Serif/Variable/OTF/Subset/NotoSerifHK-VF.otf
    URL_HASH SHA256=fa40e983d5004e88e08f5beee4177c8edbc72fe21f0f0c999fec3effd3ddccbf
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifHK-VF.otf)
FetchContent_Declare(font_NotoSerifJPVF
    URL ${_pr_noto_cjk}/Serif2.003/Serif/Variable/OTF/Subset/NotoSerifJP-VF.otf
    URL_HASH SHA256=39701fd096bc51204a8444c6c2659f007b29674a13eb62ddfa470638fe8179cd
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifJP-VF.otf)
FetchContent_Declare(font_NotoSerifKRVF
    URL ${_pr_noto_cjk}/Serif2.003/Serif/Variable/OTF/Subset/NotoSerifKR-VF.otf
    URL_HASH SHA256=0cee38f8fd687c5410089c963492ef39f9b5a64e2b11b85ce4e86105d75e0695
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoSerifKR-VF.otf)
FetchContent_Declare(font_LastResortRegular
    URL https://github.com/unicode-org/last-resort-font/releases/download/17.000/LastResort-Regular.ttf
    URL_HASH SHA256=c25223fd85c6dbb4cc720a5ae77c15bc1b5317b4488e37ba648180cbe1682a5e
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME LastResort-Regular.ttf)
FetchContent_Declare(font_NotoColorEmoji
    URL https://raw.githubusercontent.com/googlefonts/noto-emoji/v2.047/fonts/NotoColorEmoji.ttf
    URL_HASH SHA256=39ee3c587e10e89669b9ff32703261d10d5f9c4dd5ad147b6b5a1c5200591817
    DOWNLOAD_NO_EXTRACT ON
    DOWNLOAD_NAME NotoColorEmoji.ttf)
set(PR_FONT_BUNDLE_Fallback
    "font_NotoSans/NotoSans[wdth,wght].ttf"
    "font_NotoSansItalic/NotoSans-Italic[wdth,wght].ttf"
    "font_NotoSansMono/NotoSansMono[wdth,wght].ttf"
    "font_NotoSerif/NotoSerif[wdth,wght].ttf"
    "font_NotoSerifItalic/NotoSerif-Italic[wdth,wght].ttf"
    "font_NotoSansMathRegular/NotoSansMath-Regular.ttf"
    "font_NotoSansSymbols/NotoSansSymbols[wght].ttf"
    "font_NotoSansSymbols2Regular/NotoSansSymbols2-Regular.ttf"
    "font_NotoSansArabic/NotoSansArabic[wdth,wght].ttf"
    "font_NotoNaskhArabic/NotoNaskhArabic[wght].ttf"
    "font_NotoNastaliqUrdu/NotoNastaliqUrdu[wght].ttf"
    "font_NotoSansHebrew/NotoSansHebrew[wdth,wght].ttf"
    "font_NotoSerifHebrew/NotoSerifHebrew[wdth,wght].ttf"
    "font_NotoSansDevanagari/NotoSansDevanagari[wdth,wght].ttf"
    "font_NotoSerifDevanagari/NotoSerifDevanagari[wdth,wght].ttf"
    "font_NotoSansBengali/NotoSansBengali[wdth,wght].ttf"
    "font_NotoSerifBengali/NotoSerifBengali[wdth,wght].ttf"
    "font_NotoSansTamil/NotoSansTamil[wdth,wght].ttf"
    "font_NotoSerifTamil/NotoSerifTamil[wdth,wght].ttf"
    "font_NotoSerifTamilItalic/NotoSerifTamil-Italic[wdth,wght].ttf"
    "font_NotoSansThai/NotoSansThai[wdth,wght].ttf"
    "font_NotoSerifThai/NotoSerifThai[wdth,wght].ttf"
    "font_NotoSansMyanmar/NotoSansMyanmar[wdth,wght].ttf"
    "font_NotoSerifMyanmar/NotoSerifMyanmar[wdth,wght].ttf"
    "font_NotoSansKhmer/NotoSansKhmer[wdth,wght].ttf"
    "font_NotoSerifKhmer/NotoSerifKhmer[wdth,wght].ttf"
    "font_NotoSansMalayalam/NotoSansMalayalam[wdth,wght].ttf"
    "font_NotoSerifMalayalam/NotoSerifMalayalam[wght].ttf"
    "font_NotoSansKannada/NotoSansKannada[wdth,wght].ttf"
    "font_NotoSerifKannada/NotoSerifKannada[wght].ttf"
    "font_NotoSansArmenian/NotoSansArmenian[wdth,wght].ttf"
    "font_NotoSerifArmenian/NotoSerifArmenian[wdth,wght].ttf"
    "font_NotoSansGeorgian/NotoSansGeorgian[wdth,wght].ttf"
    "font_NotoSerifGeorgian/NotoSerifGeorgian[wdth,wght].ttf"
    "font_NotoSansGujarati/NotoSansGujarati[wdth,wght].ttf"
    "font_NotoSerifGujarati/NotoSerifGujarati[wght].ttf"
    "font_NotoSansGurmukhi/NotoSansGurmukhi[wdth,wght].ttf"
    "font_NotoSerifGurmukhi/NotoSerifGurmukhi[wght].ttf"
    "font_NotoSansLao/NotoSansLao[wdth,wght].ttf"
    "font_NotoSerifLao/NotoSerifLao[wdth,wght].ttf"
    "font_NotoSansMongolianRegular/NotoSansMongolian-Regular.ttf"
    "font_NotoSansOriya/NotoSansOriya[wdth,wght].ttf"
    "font_NotoSerifOriya/NotoSerifOriya[wght].ttf"
    "font_NotoSansTelugu/NotoSansTelugu[wdth,wght].ttf"
    "font_NotoSerifTelugu/NotoSerifTelugu[wght].ttf"
    "font_NotoSerifTibetan/NotoSerifTibetan[wght].ttf"
    "font_NotoSansEthiopic/NotoSansEthiopic[wdth,wght].ttf"
    "font_NotoSerifEthiopic/NotoSerifEthiopic[wdth,wght].ttf"
    "font_NotoSansSinhala/NotoSansSinhala[wdth,wght].ttf"
    "font_NotoSerifSinhala/NotoSerifSinhala[wdth,wght].ttf"
    "font_NotoSansThaana/NotoSansThaana[wght].ttf"
    "font_NotoSansSCVF/NotoSansSC-VF.otf"
    "font_NotoSansTCVF/NotoSansTC-VF.otf"
    "font_NotoSansHKVF/NotoSansHK-VF.otf"
    "font_NotoSansJPVF/NotoSansJP-VF.otf"
    "font_NotoSansKRVF/NotoSansKR-VF.otf"
    "font_NotoSerifSCVF/NotoSerifSC-VF.otf"
    "font_NotoSerifTCVF/NotoSerifTC-VF.otf"
    "font_NotoSerifHKVF/NotoSerifHK-VF.otf"
    "font_NotoSerifJPVF/NotoSerifJP-VF.otf"
    "font_NotoSerifKRVF/NotoSerifKR-VF.otf"
    "font_LastResortRegular/LastResort-Regular.ttf"
    "font_NotoColorEmoji/NotoColorEmoji.ttf")

# Noto: its generic families are in the fallback, and math is New Computer Modern's, from TeX Live 2025's final archive.
FetchContent_Declare(font_NewCM
    URL https://texlive.info/historic/systems/texlive/2025/tlnet-final/archive/newcomputermodern.tar.xz
    URL_HASH SHA256=1c94581b200952e002aa73d979680948f356af8948a5d1d7a142fb42c193ce71)
set(PR_FONT_BUNDLE_Noto
    "font_NewCM/fonts/opentype/public/newcomputermodern/NewCMMath-Regular.otf")

# Standard14: substitutes for PDF's standard 14 fonts, from documentplatform-standard14-fonts: Tinos, Arimo and Cousine,
# with the metrics of Times, Helvetica and Courier, and LeviSymbol and Levibats for Symbol and ZapfDingbats.
set(_pr_standard14
    https://raw.githubusercontent.com/levigo/documentplatform-standard14-fonts/9f7388d8129612cae823aea06dc980e045e0c092/src/main/resources/com/levigo/jadice/fonts)
FetchContent_Declare(font_TinosRegular
    URL ${_pr_standard14}/serif/Tinos-Regular.ttf
    URL_HASH SHA256=7cfa649479d3557ab03d4a39666108cd8c350a838b47387275db42e8c3a0dfb6
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_TinosBold
    URL ${_pr_standard14}/serif/Tinos-Bold.ttf
    URL_HASH SHA256=ad6657cc108b8d2e3bdcb93e038d315f16c6bb13ab40002f03d5b3bb0fde4fd4
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_TinosItalic
    URL ${_pr_standard14}/serif/Tinos-Italic.ttf
    URL_HASH SHA256=c70d58e14ee42a700e440bceebd4ed3b3198759ca0950bc587c423234f3154ab
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_TinosBoldItalic
    URL ${_pr_standard14}/serif/Tinos-BoldItalic.ttf
    URL_HASH SHA256=fe9b3436ef2feed63fbabc1531dbe0ced2b396591a02e8008dc494dce75f6952
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_ArimoRegular
    URL ${_pr_standard14}/sans/Arimo-Regular.ttf
    URL_HASH SHA256=406b00e9f6d6d2d597d51f554a944fa93358abb06c52cd6b4af2df7b5785e504
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_ArimoBold
    URL ${_pr_standard14}/sans/Arimo-Bold.ttf
    URL_HASH SHA256=385479c38638fb4bc5460b6f5e591d129936224c36033770b5e1a07febe4604e
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_ArimoItalic
    URL ${_pr_standard14}/sans/Arimo-Italic.ttf
    URL_HASH SHA256=ece3ed993939ee73f2cf0a020f038dd176a35cb8e9d6a39d7529a0d6ee5b7a41
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_ArimoBoldItalic
    URL ${_pr_standard14}/sans/Arimo-BoldItalic.ttf
    URL_HASH SHA256=468ed70dff67b0513d6b35a7de088e48bcd32bb001163838db22b3fec76b336a
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_CousineRegular
    URL ${_pr_standard14}/mono/Cousine-Regular.ttf
    URL_HASH SHA256=c21a195a437ba57d90b7331d965e7a96ac0c43ffa9aa3f39260d5b68a371de22
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_CousineBold
    URL ${_pr_standard14}/mono/Cousine-Bold.ttf
    URL_HASH SHA256=2f43ba389ec403607a1683ea3375fad03f93c84d4f1387d50716bcb9fd092d4b
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_CousineItalic
    URL ${_pr_standard14}/mono/Cousine-Italic.ttf
    URL_HASH SHA256=4896c30636c7285a5f3058b37ae15d3bd5dd7622e3c57bb9ef868061b29a7cc5
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_CousineBoldItalic
    URL ${_pr_standard14}/mono/Cousine-BoldItalic.ttf
    URL_HASH SHA256=9f90c1d0e20a192b672611e0307c0a619131e7205f5776f60593f2b89854ea95
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_LeviSymbolRegular
    URL ${_pr_standard14}/symbol/LeviSymbol-Regular.ttf
    URL_HASH SHA256=8ccc92e79327742c0cc9a8e8ccfa48e569566208d753169140572bd609ff4957
    DOWNLOAD_NO_EXTRACT ON)
FetchContent_Declare(font_LevibatsRegular
    URL ${_pr_standard14}/symbol/Levibats-Regular.ttf
    URL_HASH SHA256=517da7ca1fd10b3f0b8a1c0edbc51d57022677c256f574999f4ad1b633e8c0d0
    DOWNLOAD_NO_EXTRACT ON)
set(PR_FONT_BUNDLE_Standard14
    "font_TinosRegular/Tinos-Regular.ttf"
    "font_TinosBold/Tinos-Bold.ttf"
    "font_TinosItalic/Tinos-Italic.ttf"
    "font_TinosBoldItalic/Tinos-BoldItalic.ttf"
    "font_ArimoRegular/Arimo-Regular.ttf"
    "font_ArimoBold/Arimo-Bold.ttf"
    "font_ArimoItalic/Arimo-Italic.ttf"
    "font_ArimoBoldItalic/Arimo-BoldItalic.ttf"
    "font_CousineRegular/Cousine-Regular.ttf"
    "font_CousineBold/Cousine-Bold.ttf"
    "font_CousineItalic/Cousine-Italic.ttf"
    "font_CousineBoldItalic/Cousine-BoldItalic.ttf"
    "font_LeviSymbolRegular/LeviSymbol-Regular.ttf"
    "font_LevibatsRegular/Levibats-Regular.ttf")

function(pr_font_bundle_fetch directory)
    set(files ${PR_FONT_BUNDLE_Fallback})
    foreach (bundle IN LISTS ARGN)
        if (NOT DEFINED PR_FONT_BUNDLE_${bundle})
            message(FATAL_ERROR "pr_font_bundle_fetch: no font bundle named ${bundle}")
        endif()
        list(APPEND files ${PR_FONT_BUNDLE_${bundle}})
    endforeach()
    file(MAKE_DIRECTORY "${directory}")
    # Each file is its content's name, then its path in that content.
    foreach (file IN LISTS files)
        string(FIND "${file}" "/" slash)
        string(SUBSTRING "${file}" 0 ${slash} content)
        math(EXPR slash "${slash} + 1")
        string(SUBSTRING "${file}" ${slash} -1 path)
        FetchContent_MakeAvailable(${content})
        string(TOLOWER "${content}" content)
        get_filename_component(name "${path}" NAME)
        # Windows makes a symbolic link only with Developer Mode or elevation, and copies otherwise.
        file(CREATE_LINK "${${content}_SOURCE_DIR}/${path}" "${directory}/${name}" SYMBOLIC COPY_ON_ERROR)
    endforeach()
endfunction()
