#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace radiance::dh {
using CoverageCell = std::array<int32_t,4>;
inline int32_t wrapSection(int32_t value, int32_t size) { int32_t r=value%size; return r<0?r+size:r; }
inline size_t coverageIndex(const CoverageCell& grid, int32_t x, int32_t y, int32_t z) {
    return 1 + wrapSection(x,grid[0]) + static_cast<size_t>(wrapSection(z,grid[2]))*grid[0]
        + static_cast<size_t>(y-grid[3])*grid[0]*grid[2];
}
inline std::vector<CoverageCell> emptyCoverage(CoverageCell grid) {
    if (grid[0]<=0 || grid[1]<=0 || grid[2]<=0) return {CoverageCell{}};
    std::vector<CoverageCell> cells(1 + static_cast<size_t>(grid[0])*grid[1]*grid[2]); cells[0]=grid; return cells;
}
inline void markCoverage(std::vector<CoverageCell>& cells, int32_t x, int32_t y, int32_t z, bool ready) {
    const auto& grid=cells[0];
    if (!ready || grid[0]<=0 || grid[2]<=0 || y<grid[3] || y>=grid[3]+grid[1]) return;
    cells[coverageIndex(grid,x,y,z)]={x,y,z,1};
}
inline bool hasCoverage(const std::vector<CoverageCell>& cells, int32_t x, int32_t y, int32_t z) {
    const auto& grid=cells[0];
    if (grid[0]<=0 || grid[2]<=0 || y<grid[3] || y>=grid[3]+grid[1]) return false;
    return cells[coverageIndex(grid,x,y,z)]==CoverageCell{x,y,z,1};
}
}
