#pragma once

#include <cstddef>

#include "thermal_frame.hpp"

// Returns false only for the complete dual-plane startup placeholder.
// Malformed frames and invalid temperature samples throw std::runtime_error.
bool decodeTr256iFrame(const unsigned char *data, std::size_t size,
                       ThermalFrame &frame);
