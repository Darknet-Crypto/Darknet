package=native_rust
$(package)_version=1.42.0
$(package)_download_path=https://depends.pivx.org
$(package)_file_name_linux=rust-$($(package)_version)-x86_64-unknown-linux-gnu.tar.gz
$(package)_sha256_hash_linux=5fed7d705e215fc129c4ace8060b5dc1a47e88228ce0249d48f30d769fcb6fe3
$(package)_file_name_darwin=rust-$($(package)_version)-x86_64-apple-darwin.tar.gz
$(package)_sha256_hash_darwin=db1055c46e0d54b99da05e88c71fea21b3897e74a4f5ff9390e934f3f050c0a8
$(package)_file_name_freebsd=rust-$($(package)_version)-x86_64-unknown-freebsd.tar.gz
$(package)_sha256_hash_freebsd=230bcf17e4383fba85d3c87fe25d17737459fe561a5f4668fe70dcac2da4e17c

# Mapping from GCC canonical hosts to Rust targets
# If a mapping is not present, we assume they are identical, unless $host_os is
# "darwin", in which case we assume x86_64-apple-darwin.
$(package)_rust_target_x86_64-w64-mingw32=x86_64-pc-windows-gnu
$(package)_rust_target_i686-pc-linux-gnu=i686-unknown-linux-gnu
$(package)_rust_target_riscv64-linux-gnu=riscv64gc-unknown-linux-gnu
$(package)_rust_target_riscv64-unknown-linux-gnu=riscv64gc-unknown-linux-gnu
$(package)_rust_target_x86_64-linux-gnu=x86_64-unknown-linux-gnu
$(package)_rust_target_x86_64-pc-linux-gnu=x86_64-unknown-linux-gnu

# Mapping from Rust targets to SHA-256 hashes
$(package)_rust_std_sha256_hash_arm-unknown-linux-gnueabihf=f91d28115a46eb9af5bb73bb776269534e02add1125346b2dd7e29ecf34da892
$(package)_rust_std_sha256_hash_aarch64-unknown-linux-gnu=782b2ab52d062ecc7077ddbfff1f21b553ac21845688a75aed45d70214a3c3e7
$(package)_rust_std_sha256_hash_i686-unknown-linux-gnu=58191b8ccc78bbd288285a8e9b03c5e473ffbdf9d79c7262130202454f17c41f
$(package)_rust_std_sha256_hash_x86_64-unknown-linux-gnu=016c9619bdbb8023876579ed97ade2f8b049696c2163f7518328b77b2c274f25
$(package)_rust_std_sha256_hash_riscv64gc-unknown-linux-gnu=5496c43c340dfbef0a335498752e4efeff4534ada9a96754d9660be98ae8dc41
$(package)_rust_std_sha256_hash_x86_64-apple-darwin=1d61e9ed5d29e1bb4c18e13d551c6d856c73fb8b410053245dc6e0d3b3a0e92c
$(package)_rust_std_sha256_hash_x86_64-pc-windows-gnu=8a8389f3860df6f42fbf8b76a62ddc7b9b6fe6d0fb526dcfc42faab1005bfb6d

define rust_target
$(if $($(1)_rust_target_$(2)),$($(1)_rust_target_$(2)),$(if $(findstring darwin,$(3)),x86_64-apple-darwin,$(2)))
endef

ifneq ($(canonical_host),$(build))
$(package)_rust_target=$(call rust_target,$(package),$(canonical_host),$(host_os))
$(package)_exact_file_name=rust-std-$($(package)_version)-$($(package)_rust_target).tar.gz
$(package)_exact_sha256_hash=$($(package)_rust_std_sha256_hash_$($(package)_rust_target))
$(package)_build_subdir=buildos
$(package)_extra_sources=$($(package)_file_name_$(build_os))

define $(package)_fetch_cmds
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_download_file),$($(package)_file_name),$($(package)_sha256_hash)) && \
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name_$(build_os)),$($(package)_file_name_$(build_os)),$($(package)_sha256_hash_$(build_os)))
endef

define $(package)_extract_cmds
  mkdir -p $($(package)_extract_dir) && \
  echo "$($(package)_sha256_hash)  $($(package)_source)" > $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  echo "$($(package)_sha256_hash_$(build_os))  $($(package)_source_dir)/$($(package)_file_name_$(build_os))" >> $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  mkdir $(canonical_host) && \
  tar --strip-components=1 -xf $($(package)_source) -C $(canonical_host) && \
  mkdir buildos && \
  tar --strip-components=1 -xf $($(package)_source_dir)/$($(package)_file_name_$(build_os)) -C buildos
endef

define $(package)_stage_cmds
  bash ./install.sh --destdir=$($(package)_staging_dir) --prefix=$(build_prefix) --disable-ldconfig && \
  ../$(canonical_host)/install.sh --destdir=$($(package)_staging_dir) --prefix=$(build_prefix) --disable-ldconfig
endef
else

define $(package)_stage_cmds
  bash ./install.sh --destdir=$($(package)_staging_dir) --prefix=$(build_prefix) --disable-ldconfig
endef
endif
