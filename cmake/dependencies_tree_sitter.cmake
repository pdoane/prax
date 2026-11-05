# What lib/syntax's tree-sitter backend fetches: tree-sitter, built from its one-source library, the grammars of the
# languages that the backend builds in, each a dependency named tree_sitter_<grammar>, and the queries that it takes
# from nvim-treesitter.

pr_dependency(tree_sitter
    URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.27.1.tar.gz
    URL_HASH SHA256=982cd3d4d9eb7be18c243240a622423fd2e4ecb4bec2cd98832c2f3fa1f0f333
    SOURCE_SUBDIR none
)

pr_dependency(tree_sitter_bash
    URL https://github.com/tree-sitter/tree-sitter-bash/archive/v0.25.1.tar.gz
    URL_HASH SHA256=2e785a761225b6c433410ef9c7b63cfb0a4e83a35a19e0f2aec140b42c06b52d
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_c
    URL https://github.com/tree-sitter/tree-sitter-c/archive/v0.24.2.tar.gz
    URL_HASH SHA256=2eeb4db31f8fa0865e45488503d13403923bcb485a1bdb637abff8c42dd97364
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_c_sharp
    URL https://github.com/tree-sitter/tree-sitter-c-sharp/archive/v0.23.5.tar.gz
    URL_HASH SHA256=9628b164369071019368618bdefa446f0aab8acaac47b75d5dfb209e93b8903b
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_cmake
    URL https://github.com/uyha/tree-sitter-cmake/archive/v0.7.6.tar.gz
    URL_HASH SHA256=388129103b886be0f2191c9ddda6ddcee1e21c284838a0e86cc538431c6759c3
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_cpp
    URL https://github.com/tree-sitter/tree-sitter-cpp/archive/v0.23.4.tar.gz
    URL_HASH SHA256=7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_css
    URL https://github.com/tree-sitter/tree-sitter-css/archive/v0.25.0.tar.gz
    URL_HASH SHA256=03965344d8c0435dc54fb45b281578420bb7db8b99df4d34e7e74105a274cb79
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_dart
    URL https://github.com/UserNobody14/tree-sitter-dart/archive/0038280b774b65c1b89ba7fec50624d8baed9fe1.tar.gz
    URL_HASH SHA256=7d9f51b4ff20756a21935ce69a2386e6db1e2fda325835d41790e745270e0db8
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_diff
    URL https://github.com/tree-sitter-grammars/tree-sitter-diff/archive/v0.2.0.tar.gz
    URL_HASH SHA256=6092f7fd11e186d96d3c130fdbe8d03cfc426ee9cc62cf4a2112571b1b0372e6
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_dockerfile
    URL https://github.com/camdencheek/tree-sitter-dockerfile/archive/v0.2.0.tar.gz
    URL_HASH SHA256=8cbdf50838cc55e841aa9585a1a09e5b8c41454ae30ce0a93a7bd71adb140818
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_glsl
    URL https://github.com/tree-sitter-grammars/tree-sitter-glsl/archive/v0.2.0.tar.gz
    URL_HASH SHA256=65d098362f9ffd47e9bf5916133ed24ccf934b01027debdaeeb46ff3ec41caf0
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_go
    URL https://github.com/tree-sitter/tree-sitter-go/archive/v0.25.0.tar.gz
    URL_HASH SHA256=2dc241b97872c53195e01b86542b411a3c1a6201d9c946c78d5c60c063bba1ef
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_hcl
    URL https://github.com/tree-sitter-grammars/tree-sitter-hcl/archive/v1.2.0.tar.gz
    URL_HASH SHA256=a95bc6b00271e08dee9e63b895b2563dac802db0015fdf5a24a1b0244f2cb560
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_hlsl
    URL https://github.com/tree-sitter-grammars/tree-sitter-hlsl/archive/v0.2.0.tar.gz
    URL_HASH SHA256=915ec84ddde4f5cf11f735de6f3e96ccf3355222f98d4d1e84436a743a519737
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_html
    URL https://github.com/tree-sitter/tree-sitter-html/archive/v0.23.2.tar.gz
    URL_HASH SHA256=21fa4f2d4dcb890ef12d09f4979a0007814f67f1c7294a9b17b0108a09e45ef7
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_java
    URL https://github.com/tree-sitter/tree-sitter-java/archive/v0.23.5.tar.gz
    URL_HASH SHA256=cb199e0faae4b2c08425f88cbb51c1a9319612e7b96315a174a624db9bf3d9f0
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_javascript
    URL https://github.com/tree-sitter/tree-sitter-javascript/archive/v0.25.0.tar.gz
    URL_HASH SHA256=9712fc283d3dc01d996d20b6392143445d05867a7aad76fdd723824468428b86
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_json
    URL https://github.com/tree-sitter/tree-sitter-json/archive/v0.24.8.tar.gz
    URL_HASH SHA256=acf6e8362457e819ed8b613f2ad9a0e1b621a77556c296f3abea58f7880a9213
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_kotlin
    URL https://github.com/fwcd/tree-sitter-kotlin/archive/0.3.8.tar.gz
    URL_HASH SHA256=7dd60975786bf9cb4be6a5176f5ccb5fed505f9929a012da30762505b1015669
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_lua
    URL https://github.com/tree-sitter-grammars/tree-sitter-lua/archive/v0.5.0.tar.gz
    URL_HASH SHA256=cf01b93f4b61b96a6d27942cf28eeda4cbce7d503c3bef773a8930b3d778a2d9
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_make
    URL https://github.com/tree-sitter-grammars/tree-sitter-make/archive/v1.1.1.tar.gz
    URL_HASH SHA256=31affca06041623cfa5f6a728cb3f0f2a8eb7fd01607e1a2681ea1f318367211
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_markdown
    URL https://github.com/tree-sitter-grammars/tree-sitter-markdown/archive/v0.5.4.tar.gz
    URL_HASH SHA256=481ce2c1cce7b1130bc8455c186052bd0fa461f1ff488666994c6a3da7fca2ac
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_objc
    URL https://github.com/tree-sitter-grammars/tree-sitter-objc/archive/v3.0.2.tar.gz
    URL_HASH SHA256=186d03ecb9ae41cde85efbe283dcbe67c277fff766a946f7efd6d51fee72370d
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_php
    URL https://github.com/tree-sitter/tree-sitter-php/archive/v0.25.1.tar.gz
    URL_HASH SHA256=1377ac7d40474e002af12fe098287e065fac6d818d43376dc6617dbc94836ad8
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_powershell
    URL https://github.com/airbus-cert/tree-sitter-powershell/archive/v0.26.6.tar.gz
    URL_HASH SHA256=ef50282d95bb66fac71e0ae65308358a5f4bd1da1606cb01149b201eb92c42c5
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_python
    URL https://github.com/tree-sitter/tree-sitter-python/archive/v0.25.0.tar.gz
    URL_HASH SHA256=4609a3665a620e117acf795ff01b9e965880f81745f287a16336f4ca86cf270c
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_ruby
    URL https://github.com/tree-sitter/tree-sitter-ruby/archive/v0.23.1.tar.gz
    URL_HASH SHA256=e7e49577ddc1f2de8e42d42353b477e338c15bbb95b2558e123ddc13d88789f0
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_rust
    URL https://github.com/tree-sitter/tree-sitter-rust/archive/v0.24.2.tar.gz
    URL_HASH SHA256=061e90a539a55a6aa65dceb0ad6425c50ab1a6e3e6d4ba430e2795ed4550f10e
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_slang
    URL https://github.com/tree-sitter-grammars/tree-sitter-slang/archive/v0.3.1.tar.gz
    URL_HASH SHA256=81c5ca98baa698071294d5d7f71c11e77000501a3c0f7a8cd0fec08e117c09ff
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_swift
    URL https://github.com/alex-pinkus/tree-sitter-swift/archive/0.7.4-with-generated-files.tar.gz
    URL_HASH SHA256=bf4d34383638f41cde398f4678dea72cdcda8442c7cfd13944f4bb63e9a96f70
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_toml
    URL https://github.com/tree-sitter-grammars/tree-sitter-toml/archive/v0.7.0.tar.gz
    URL_HASH SHA256=7d52a7d4884f307aabc872867c69084d94456d8afcdc63b0a73031a8b29036dc
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_typescript
    URL https://github.com/tree-sitter/tree-sitter-typescript/archive/v0.23.2.tar.gz
    URL_HASH SHA256=2c4ce711ae8d1218a3b2f899189298159d672870b5b34dff5d937bed2f3e8983
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_wgsl
    URL https://github.com/szebniok/tree-sitter-wgsl/archive/40259f3c77ea856841a4e0c4c807705f3e4a2b65.tar.gz
    URL_HASH SHA256=ddd4a41dacb45e8c58a304312cb68994a8be65b6681c5402ef4492a05df7c407
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_xml
    URL https://github.com/tree-sitter-grammars/tree-sitter-xml/archive/v0.7.0.tar.gz
    URL_HASH SHA256=4330a6b3685c2f66d108e1df0448eb40c468518c3a66f2c1607a924c262a3eb9
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_yaml
    URL https://github.com/tree-sitter-grammars/tree-sitter-yaml/archive/v0.7.2.tar.gz
    URL_HASH SHA256=aeaff5731bb8b66c7054c8aed33cd5edea5f4cd2ac71654f3f6c2ba2073d8fac
    SOURCE_SUBDIR none
)

# The release's archive, which holds the generated parser that the tag's does not.
pr_dependency(tree_sitter_sql
    URL https://github.com/DerekStride/tree-sitter-sql/releases/download/v0.3.11/tree-sitter-sql-v0.3.11.tar.gz
    URL_HASH SHA256=a97a324eae9c81ed68f6e162b9b33f8911fc6442caa2950e57c498e2460d1387
    SOURCE_SUBDIR none
)

# nvim-treesitter's queries, for the grammars that ship none of their own (HCL, HLSL, Slang). They are written for
# Neovim, whose predicates the backend reads as well as tree-sitter's.
pr_dependency(nvim_treesitter
    URL https://github.com/nvim-treesitter/nvim-treesitter/archive/610734c5d3e59761d5f79318d120633374537fa5.tar.gz
    URL_HASH SHA256=86a819855894e7f622163ac519621eded615ff9efa831ec425a3588c4ac68cd7
    SOURCE_SUBDIR none
)
