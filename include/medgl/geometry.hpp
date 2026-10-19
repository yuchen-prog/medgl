#pragma once

#include "medgl/renderer.hpp"

#include <array>

namespace medgl {

using Point3d = std::array<double, 3>;
using Affine4d = std::array<double, 16>;

enum class StandardView { Anterior, Posterior, Left, Right, Superior, Inferior };

void ValidateGeometry(const VolumeData& volume);
Affine4d VoxelToPatientAffine(const VolumeData& volume);
Point3d VoxelToPatient(const VolumeData& volume, Point3d voxel);
Point3d PatientToVoxel(const VolumeData& volume, Point3d patient);
std::array<float, 4> StandardViewRotation(StandardView view);

}