#!/bin/bash
# WP2: reconstruct third_party/llama.cpp (gitignored -- 206 MiB) from the
# spike's pinned clone and apply the residctl mmap hook. Then build.
set -eu
LC=/root/residctl/third_party/llama.cpp
SPIKE=/root/spike/src/llama.cpp

if [ ! -d "$LC" ]; then
    mkdir -p /root/residctl/third_party
    cp -r "$SPIKE" "$LC"
    rm -rf "$LC/.git"
fi

# apply the mmap hook if not already applied
if ! grep -q residctl_llama_mmap "$LC/src/llama-mmap.cpp"; then
    patch -p0 -d "$LC" < /root/residctl/src/wp2_llama_mmap.patch
fi

cmake -S "$LC" -B "$LC/build" -DGGML_CUDA=OFF -DGGML_NATIVE=ON -DLLAMA_CURL=OFF \
      -DCMAKE_BUILD_TYPE=Release -DLLAMA_BUILD_TESTS=OFF
cmake --build "$LC/build" -j"$(nproc)" --target llama llama-bench

echo "third_party/llama.cpp ready. Now run scripts/build-real-model-integration.sh"
