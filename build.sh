#!/usr/bin/env bash
# Build both custom components from bridge.c and layer.c.
# Requires Python 3, GCC, Vulkan development/utility headers, Clang, lld-link
# and Wine's Windows x64 kernel32 import library. Nothing is installed globally.
# Override the three FSR_PROBE_* paths below for a different toolchain layout.
set -euo pipefail
cd -- "$(dirname -- "$0")"
compiler=${FSR_PROBE_CLANG:-/usr/lib64/rocm/llvm/bin/clang}
linker=${FSR_PROBE_LINKER:-/usr/lib64/rocm/llvm/bin/lld-link}
kernel32_import=${FSR_PROBE_KERNEL32_IMPORT:-/opt/wine-staging/lib64/wine/x86_64-windows/libkernel32.a}

for tool in "$compiler" "$linker" gcc python3; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'Required build tool not found: %s\n' "$tool" >&2
    exit 1
  fi
done
for dependency in "$kernel32_import" /usr/include/vulkan/vulkan_core.h /usr/include/vulkan/vk_layer.h /usr/include/vulkan/utility/vk_struct_helper.hpp; do
  if [ ! -r "$dependency" ]; then
    printf 'Required development file not found: %s\n' "$dependency" >&2
    exit 1
  fi
done

mkdir -p build

# Windows x64 bridge, loaded by OptiScaler through Wine.
"$compiler" --target=x86_64-w64-windows-gnu -std=c11 -O1 -ffreestanding \
  -fno-builtin -fno-stack-protector -mno-stack-arg-probe -Wall -Wextra \
  -Werror -Wno-missing-field-initializers -idirafter /usr/include \
  -c bridge.c -o build/ffx_native_bridge.obj
"$linker" /machine:x64 /dll /noentry /nodefaultlib \
  /out:build/amd_fidelityfx_vk.dll build/ffx_native_bridge.obj \
  "$kernel32_import"

# Generate exact Vulkan pNext structure sizes from the installed headers.
# This is a generated build file, not another source file to maintain.
python3 - <<'PY'
import re
import subprocess
from pathlib import Path

core = Path('/usr/include/vulkan/vulkan_core.h').read_text()
helper = Path('/usr/include/vulkan/utility/vk_struct_helper.hpp').read_text()
structs = set(re.findall(
    r'typedef struct (\w+)\s*\{\s*VkStructureType\s+sType;\s*(?:const\s+)?void\s*\*\s*pNext;', core))
preprocessed = subprocess.check_output(
    ['gcc', '-E', '-P', '-x', 'c', '-include', 'vulkan/vulkan.h', '/dev/null'], text=True)
enum_tags = set(re.findall(r'\b(VK_STRUCTURE_TYPE_\w+)\s*=', preprocessed))
mapping = re.findall(
    r'GetSType<(\w+)>\(\)\s*\{\s*return\s+(VK_STRUCTURE_TYPE_\w+);', helper)
seen = set()
lines = [
    '/* Generated from Khronos vulkan_core.h and vk_struct_helper.hpp. */',
    'static size_t chain_node_size(VkStructureType type) {',
    '    switch(type) {',
    '    case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO: return sizeof(VkLayerDeviceCreateInfo);',
]
for name, tag in mapping:
    if name in structs and tag not in seen and tag in enum_tags:
        seen.add(tag)
        lines.append(f'    case {tag}: return sizeof({name});')
lines += ['    default: return 0;', '    }', '}']
Path('build/vk_chain_sizes.h').write_text('\n'.join(lines) + '\n')
print('Generated exact sizes for', len(seen), 'core pNext structure tags')
PY

# Native Linux helper with an embedded manifest and LD_PRELOAD bootstrap.
# Hidden hooks prevent accidental Vulkan interposition before the loader runs.
gcc -std=c11 -shared -fPIC -fvisibility=hidden -O2 -Wall -Wextra -Werror -Wno-misleading-indentation \
  -Ibuild layer.c -o build/libnative_fsr4_layer.so -lpthread -ldl -Wl,-Bsymbolic
printf 'Built build/amd_fidelityfx_vk.dll and build/libnative_fsr4_layer.so\n'
printf '%s\n' \
  'No distributed layer JSON, controls file or launcher script is needed.' \
  'Copy the new bridge DLL and .so beside the three AMD companion/provider DLLs.' \
  'Keep the tested OptiScaler.ini native Vulkan settings.' \
  'LD_PRELOAD="libnative_fsr4_layer.so${LD_PRELOAD:+:$LD_PRELOAD}" %command%'
