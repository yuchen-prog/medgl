#include "medgl/geometry.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void Near(double actual, double expected, const char* message)
{
    if (std::abs(actual - expected) > 1e-5) throw std::runtime_error(message);
}

medgl::Point3d Rotate(std::array<float, 4> quaternion, medgl::Point3d vector)
{
    const medgl::Point3d imaginary{quaternion[0], quaternion[1], quaternion[2]};
    const medgl::Point3d cross{2 * (imaginary[1] * vector[2] - imaginary[2] * vector[1]),
        2 * (imaginary[2] * vector[0] - imaginary[0] * vector[2]),
        2 * (imaginary[0] * vector[1] - imaginary[1] * vector[0])};
    return {vector[0] + quaternion[3] * cross[0] + imaginary[1] * cross[2] - imaginary[2] * cross[1],
        vector[1] + quaternion[3] * cross[1] + imaginary[2] * cross[0] - imaginary[0] * cross[2],
        vector[2] + quaternion[3] * cross[2] + imaginary[0] * cross[1] - imaginary[1] * cross[0]};
}

}

int main()
{
    try {
        medgl::VolumeData volume;
        volume.shape = {11, 13, 17};
        volume.spacing = {0.5f, 2, 3};
        volume.origin = {35, -12, 80};
        volume.direction = {0.6f, -0.8f, 0, 0.8f, 0.6f, 0, 0, 0, 1};
        const auto patient = medgl::VoxelToPatient(volume, {2, 3, 4});
        Near(patient[0], 30.8, "Affine row/column interpretation is incorrect");
        Near(patient[1], -7.6, "Patient Y is incorrect");
        Near(patient[2], 92, "Patient Z is incorrect");
        for (const medgl::Point3d point : {medgl::Point3d{0, 0, 0}, {10, 12, 16}, {2.25, 3.75, 4.5}, {-0.5, -0.5, -0.5}}) {
            const auto roundTrip = medgl::PatientToVoxel(volume, medgl::VoxelToPatient(volume, point));
            for (int axis = 0; axis < 3; ++axis) Near(roundTrip[axis], point[axis], "Patient/voxel round trip failed");
        }
        const auto affine = medgl::VoxelToPatientAffine(volume);
        Near(affine[0], 0.3, "Affine X spacing is incorrect");
        Near(affine[1], -1.6, "Affine Y spacing is incorrect");
        Near(affine[3], 35, "Affine translation is incorrect");
        medgl::VolumeData oversized;
        oversized.shape = {2, 2, 2};
        oversized.spacing = {2, 2, 2};
        bool forwardRejected = false;
        try { medgl::VoxelToPatient(oversized, {std::numeric_limits<double>::max(), 0, 0}); }
        catch (const std::overflow_error&) { forwardRejected = true; }
        if (!forwardRejected) throw std::runtime_error("Non-finite patient output was returned");
        oversized.spacing = {std::numeric_limits<float>::min(), 1, 1};
        bool inverseRejected = false;
        try { medgl::PatientToVoxel(oversized, {std::numeric_limits<double>::max(), 0, 0}); }
        catch (const std::overflow_error&) { inverseRejected = true; }
        if (!inverseRejected) throw std::runtime_error("Non-finite voxel output was returned");
        const medgl::StandardView views[]{medgl::StandardView::Anterior, medgl::StandardView::Posterior,
            medgl::StandardView::Left, medgl::StandardView::Right, medgl::StandardView::Superior, medgl::StandardView::Inferior};
        const medgl::Point3d forwards[]{{0, 1, 0}, {0, -1, 0}, {-1, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 0, 1}};
        const medgl::Point3d rights[]{{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
        for (int index = 0; index < 6; ++index) {
            const auto rotation = medgl::StandardViewRotation(views[index]);
            const auto forward = Rotate(rotation, {0, 0, 1});
            const auto right = Rotate(rotation, {1, 0, 0});
            const auto up = Rotate(rotation, {0, 1, 0});
            for (int axis = 0; axis < 3; ++axis) {
                Near(forward[axis], forwards[index][axis], "Standard view ray is incorrect");
                Near(right[axis], rights[index][axis], "Standard view is mirrored");
                Near(up[axis], index < 4 ? (axis == 2 ? 1 : 0) : (axis == 1 ? 1 : 0), "Standard view up is incorrect");
            }
        }
        std::cout << "[PASS] LPS affine, anisotropic/oblique round trips and six camera bases\n";
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
    return 0;
}