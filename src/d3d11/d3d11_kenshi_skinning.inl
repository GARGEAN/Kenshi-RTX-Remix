// Included inside namespace dxvk after native palette decoding. Reads existing CPU shadows, never a
// GPU readback. The full vertex span is deliberately conservative when a draw uses an index subset.
struct KenshiBoneUsage {
  uint64_t mask = 0;
  XXH64_hash_t weightsHash = 0;
};

static KenshiBoneUsage scanKenshiBoneUsage(const uint8_t* weights,
    size_t weightStride, const uint8_t* indices, size_t indexStride,
    uint32_t vertices, uint32_t influences, uint32_t paletteSize) {
  KenshiBoneUsage result;
  if (weights == nullptr || indices == nullptr || vertices == 0u ||
      influences == 0u || influences > 3u || paletteSize == 0u || paletteSize > 64u)
    return result;
  for (uint32_t vertex = 0u; vertex < vertices; ++vertex) {
    for (uint32_t influence = 0u; influence < influences; ++influence) {
      float weight;
      std::memcpy(&weight, weights + size_t(vertex) * weightStride + influence * 4u, 4u);
      if (!std::isfinite(weight)) return {};
      // Hash only what the shader reads, including the weight values.
      result.weightsHash = XXH3_64bits_withSeed(&weight, sizeof(weight), result.weightsHash);
      if (weight == 0.f) continue;
      const uint8_t bone = indices[size_t(vertex) * indexStride + influence];
      if (bone >= paletteSize) return {};
      result.mask |= uint64_t(1) << bone;
      result.weightsHash = XXH3_64bits_withSeed(&bone, sizeof(bone), result.weightsHash);
    }
  }
  return result;
}

static void applyKenshiBoneUsage(const D3D11Buffer* weights, size_t weightOffset,
    uint32_t weightStride, const D3D11Buffer* indices, size_t indexOffset,
    uint32_t indexStride, uint32_t vertices, SkinningData& skin) {
  skin.paletteHash = skin.boneHash;
  if (!skin.explicitWeights || weights == nullptr || indices == nullptr ||
      vertices == 0u || weightStride < skin.numBonesPerVertex * 4u || indexStride < 4u ||
      skin.numBones > 64u)
    return;
  const size_t weightBytes = size_t(vertices - 1u) * weightStride + skin.numBonesPerVertex * 4u;
  const size_t indexBytes = size_t(vertices - 1u) * indexStride + 4u;
  const auto* weightData = static_cast<const uint8_t*>(weights->GetVertexShadow(weightOffset, weightBytes));
  const auto* indexData = static_cast<const uint8_t*>(indices->GetVertexShadow(indexOffset, indexBytes));
  if (weightData == nullptr || indexData == nullptr) return;

  // Registry tokens change on writes and retirement, including pointer reuse.
  // Epoch covers command lists/scene invalidation. Verify the full key on hits.
  const std::array<uint64_t, 9> key = {
    prepared_terrain::revision(weights), prepared_terrain::revision(indices),
    prepared_terrain::attributeEpoch.load(std::memory_order_relaxed),
    weightOffset, indexOffset, weightStride, indexStride, vertices,
    (uint64_t(skin.numBones) << 32u) | skin.numBonesPerVertex
  };
  struct Entry { std::array<uint64_t, 9> key; KenshiBoneUsage usage; };
  static thread_local std::unordered_map<XXH64_hash_t, Entry> cache;
  const auto hash = XXH3_64bits(key.data(), sizeof(key));
  auto found = cache.find(hash);
  KenshiBoneUsage usage;
  if (found != cache.end() && found->second.key == key) {
    usage = found->second.usage;
  } else {
    usage = scanKenshiBoneUsage(weightData, weightStride, indexData, indexStride,
      vertices, skin.numBonesPerVertex, skin.numBones);
    if (cache.size() >= 2048u) cache.clear();
    cache.insert_or_assign(hash, Entry{key, usage});
  }
  if (usage.mask == 0u) return; // Unknown/invalid input keeps the conservative hash.
  Matrix4 active[64];
  uint32_t count = 0u;
  for (uint32_t bone = 0u; bone < skin.numBones; ++bone) {
    if ((usage.mask & (uint64_t(1) << bone)) == 0u) continue;
    if (count == 0u) skin.minBoneIndex = bone;
    active[count++] = skin.pBoneMatrices[bone];
  }
  skin.usedBoneMask = usage.mask;
  skin.boneHash = XXH3_64bits_withSeed(active, count * sizeof(Matrix4),
    XXH3_64bits_withSeed(&usage.mask, sizeof(usage.mask), usage.weightsHash));
  // Keep numBones and every uploaded matrix: palette capacity is independent
  // of the set used for history identity and the tracking anchor.
}
