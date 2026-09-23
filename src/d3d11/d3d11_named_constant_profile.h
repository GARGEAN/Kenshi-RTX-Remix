#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dxvk {
  // V699: immutable b0 layout only. No mapped pointers, values or read results.
  class D3D11NamedConstantProfile {
  public:
    struct Location {
      uint32_t offset = 0, size = 0;
      bool used = false;
    };
    template<typename Variables>
    void initialize(const Variables& variables) {
      m_locations.clear();
      for (const auto& variable : variables)
        // Preserve the first declaration, including an unused one.
        m_locations.emplace(variable.name,
          Location { variable.offset, variable.size, variable.used });
    }
    const Location* find(const char* name) const {
      const auto found = m_locations.find(std::string_view(name));
      return found != m_locations.end() ? &found->second : nullptr;
    }
  private:
    // Views refer to the immutable reflection, retained by the owning cache entry.
    std::unordered_map<std::string_view, Location> m_locations;
  };
}
