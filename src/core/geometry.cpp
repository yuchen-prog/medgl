#include "medgl/geometry.hpp"

#include <cmath>
#include <stdexcept>

namespace medgl {

void ValidateGeometry(const VolumeData& volume)
{
    volume.shape.VoxelCount();
    for (float spacing : volume.spacing)
        if (!std::isfinite(spacing) || spacing <= 0) throw std::invalid_argument("Invalid voxel spacing");
    for (float coordinate : volume.origin)
        if (!std::isfinite(coordinate)) throw std::invalid_argument("Invalid patient origin");
    for (float component : volume.direction)
        if (!std::isfinite(component)) throw std::invalid_argument("Invalid patient direction");
    for (int first = 0; first < 3; ++first) {
        for (int second = 0; second < 3; ++second) {
            double dot = 0;
            for (int row = 0; row < 3; ++row)
                dot += static_cast<double>(volume.direction[row * 3 + first]) * volume.direction[row * 3 + second];
            if (std::abs(dot - (first == second ? 1.0 : 0.0)) > 1e-4)
                throw std::invalid_argument("Patient direction must be orthonormal");
        }
    }
}

Affine4d VoxelToPatientAffine(const VolumeData& volume)
{
    ValidateGeometry(volume);
    Affine4d affine{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            affine[row * 4 + column] = static_cast<double>(volume.direction[row * 3 + column]) * volume.spacing[column];
        affine[row * 4 + 3] = volume.origin[row];
    }
    affine[15] = 1;
    return affine;
}

Point3d VoxelToPatient(const VolumeData& volume, Point3d voxel)
{
    const auto affine = VoxelToPatientAffine(volume);
    Point3d patient{};
    for (int row = 0; row < 3; ++row) {
        patient[row] = affine[row * 4 + 3];
        for (int column = 0; column < 3; ++column) {
            if (!std::isfinite(voxel[column])) throw std::invalid_argument("Non-finite voxel coordinate");
            patient[row] += affine[row * 4 + column] * voxel[column];
        }
        if (!std::isfinite(patient[row])) throw std::overflow_error("Patient coordinate exceeds the numeric range");
    }
    return patient;
}

Point3d PatientToVoxel(const VolumeData& volume, Point3d patient)
{
    const auto affine = VoxelToPatientAffine(volume);
    const double determinant = affine[0] * (affine[5] * affine[10] - affine[6] * affine[9])
        - affine[1] * (affine[4] * affine[10] - affine[6] * affine[8])
        + affine[2] * (affine[4] * affine[9] - affine[5] * affine[8]);
    if (!std::isfinite(determinant) || determinant == 0) throw std::invalid_argument("Singular patient affine");
    const std::array<double, 9> inverse{
        (affine[5] * affine[10] - affine[6] * affine[9]) / determinant,
        (affine[2] * affine[9] - affine[1] * affine[10]) / determinant,
        (affine[1] * affine[6] - affine[2] * affine[5]) / determinant,
        (affine[6] * affine[8] - affine[4] * affine[10]) / determinant,
        (affine[0] * affine[10] - affine[2] * affine[8]) / determinant,
        (affine[2] * affine[4] - affine[0] * affine[6]) / determinant,
        (affine[4] * affine[9] - affine[5] * affine[8]) / determinant,
        (affine[1] * affine[8] - affine[0] * affine[9]) / determinant,
        (affine[0] * affine[5] - affine[1] * affine[4]) / determinant};
    Point3d delta{};
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(patient[axis])) throw std::invalid_argument("Non-finite patient coordinate");
        delta[axis] = patient[axis] - volume.origin[axis];
    }
    Point3d voxel{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) voxel[row] += inverse[row * 3 + column] * delta[column];
    for (double coordinate : voxel)
        if (!std::isfinite(coordinate)) throw std::overflow_error("Voxel coordinate exceeds the numeric range");
    return voxel;
}

std::array<float, 4> StandardViewRotation(StandardView view)
{
    const float half = std::sqrt(0.5f);
    switch (view) {
    case StandardView::Anterior: return {0, half, half, 0};
    case StandardView::Posterior: return {half, 0, 0, half};
    case StandardView::Left: return {0.5f, -0.5f, -0.5f, 0.5f};
    case StandardView::Right: return {0.5f, 0.5f, 0.5f, 0.5f};
    case StandardView::Superior: return {0, 1, 0, 0};
    case StandardView::Inferior: return {0, 0, 0, 1};
    }
    throw std::invalid_argument("Unknown standard view");
}

}