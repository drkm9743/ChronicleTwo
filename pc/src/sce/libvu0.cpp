// libvu0 on the host FPU. Each function does what the SDK's macro-mode VU0
// code does, component for component: the same products in the same order,
// the same w handling (InversMatrix, for one, assumes a rotation and a
// translation, as Sony's does). Results can still differ from the PS2 in the
// last bit, since the VU rounds toward zero and has no denormals.

#include <cmath>
#include <cstring>

#include <libvu0.h>

namespace {

inline void Mul(sceVu0FMATRIX out, sceVu0FMATRIX a, sceVu0FMATRIX b) {
    // out = a * b, column-major: column i of out is a applied to column i of b.
    sceVu0FMATRIX r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i][j] = a[0][j] * b[i][0] + a[1][j] * b[i][1] + a[2][j] * b[i][2] + a[3][j] * b[i][3];
        }
    }
    std::memcpy(out, r, sizeof(r));
}

inline int Ftoi(float f) {
    // VU FTOI truncates toward zero and saturates.
    if (f >= 2147483647.0f) {
        return 0x7FFFFFFF;
    }
    if (f <= -2147483648.0f) {
        return int(0x80000000u);
    }
    if (f != f) {
        return 0;
    }
    return int(f);
}

} // namespace

extern "C" {

void sceVu0CopyVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1) { std::memmove(v0, v1, sizeof(sceVu0FVECTOR)); }

void sceVu0CopyVectorXYZ(sceVu0FVECTOR v0, sceVu0FVECTOR v1) {
    v0[0] = v1[0];
    v0[1] = v1[1];
    v0[2] = v1[2];
}

void sceVu0FTOI0Vector(sceVu0IVECTOR v0, sceVu0FVECTOR v1) {
    for (int i = 0; i < 4; i++) {
        v0[i] = Ftoi(v1[i]);
    }
}

void sceVu0FTOI4Vector(sceVu0IVECTOR v0, sceVu0FVECTOR v1) {
    for (int i = 0; i < 4; i++) {
        v0[i] = Ftoi(v1[i] * 16.0f);
    }
}

void sceVu0ITOF0Vector(sceVu0FVECTOR v0, sceVu0IVECTOR v1) {
    for (int i = 0; i < 4; i++) {
        v0[i] = float(v1[i]);
    }
}

void sceVu0ITOF4Vector(sceVu0FVECTOR v0, sceVu0IVECTOR v1) {
    for (int i = 0; i < 4; i++) {
        v0[i] = float(v1[i]) * 0.0625f;
    }
}

void sceVu0ScaleVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1, float s) {
    for (int i = 0; i < 4; i++) {
        v0[i] = v1[i] * s;
    }
}

void sceVu0ScaleVectorXYZ(sceVu0FVECTOR v0, sceVu0FVECTOR v1, float s) {
    v0[0] = v1[0] * s;
    v0[1] = v1[1] * s;
    v0[2] = v1[2] * s;
    v0[3] = v1[3];
}

void sceVu0AddVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1, sceVu0FVECTOR v2) {
    for (int i = 0; i < 4; i++) {
        v0[i] = v1[i] + v2[i];
    }
}

void sceVu0SubVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1, sceVu0FVECTOR v2) {
    for (int i = 0; i < 4; i++) {
        v0[i] = v1[i] - v2[i];
    }
}

void sceVu0MulVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1, sceVu0FVECTOR v2) {
    for (int i = 0; i < 4; i++) {
        v0[i] = v1[i] * v2[i];
    }
}

void sceVu0InterVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1, sceVu0FVECTOR v2, float r) {
    float s = 1.0f - r;
    for (int i = 0; i < 4; i++) {
        v0[i] = v1[i] * r + v2[i] * s;
    }
}

void sceVu0InterVectorXYZ(sceVu0FVECTOR v0, sceVu0FVECTOR v1, sceVu0FVECTOR v2, float r) {
    float s = 1.0f - r;
    float w = v1[3];
    for (int i = 0; i < 3; i++) {
        v0[i] = v1[i] * r + v2[i] * s;
    }
    v0[3] = w;
}

void sceVu0DivVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1, float q) {
    float r = 1.0f / q;
    for (int i = 0; i < 4; i++) {
        v0[i] = v1[i] * r;
    }
}

void sceVu0DivVectorXYZ(sceVu0FVECTOR v0, sceVu0FVECTOR v1, float q) {
    float r = 1.0f / q;
    v0[0] = v1[0] * r;
    v0[1] = v1[1] * r;
    v0[2] = v1[2] * r;
    v0[3] = v1[3];
}

float sceVu0InnerProduct(sceVu0FVECTOR v0, sceVu0FVECTOR v1) { return v0[0] * v1[0] + v0[1] * v1[1] + v0[2] * v1[2]; }

void sceVu0OuterProduct(sceVu0FVECTOR v0, sceVu0FVECTOR v1, sceVu0FVECTOR v2) {
    float x = v1[1] * v2[2] - v1[2] * v2[1];
    float y = v1[2] * v2[0] - v1[0] * v2[2];
    float z = v1[0] * v2[1] - v1[1] * v2[0];
    v0[0] = x;
    v0[1] = y;
    v0[2] = z;
    v0[3] = 0.0f;
}

void sceVu0Normalize(sceVu0FVECTOR v0, sceVu0FVECTOR v1) {
    float q = 1.0f / std::sqrt(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]);
    float x = v1[0] * q, y = v1[1] * q, z = v1[2] * q;
    v0[0] = x;
    v0[1] = y;
    v0[2] = z;
    v0[3] = 0.0f;
}

void sceVu0ApplyMatrix(sceVu0FVECTOR v0, sceVu0FMATRIX m, sceVu0FVECTOR v1) {
    float r[4];
    for (int j = 0; j < 4; j++) {
        r[j] = m[0][j] * v1[0] + m[1][j] * v1[1] + m[2][j] * v1[2] + m[3][j] * v1[3];
    }
    std::memcpy(v0, r, sizeof(r));
}

void sceVu0UnitMatrix(sceVu0FMATRIX m) {
    std::memset(m, 0, sizeof(sceVu0FMATRIX));
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

void sceVu0CopyMatrix(sceVu0FMATRIX m0, sceVu0FMATRIX m1) { std::memmove(m0, m1, sizeof(sceVu0FMATRIX)); }

void sceVu0TransposeMatrix(sceVu0FMATRIX m0, sceVu0FMATRIX m1) {
    sceVu0FMATRIX r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i][j] = m1[j][i];
        }
    }
    std::memcpy(m0, r, sizeof(r));
}

void sceVu0MulMatrix(sceVu0FMATRIX m0, sceVu0FMATRIX m1, sceVu0FMATRIX m2) { Mul(m0, m1, m2); }

void sceVu0InversMatrix(sceVu0FMATRIX m0, sceVu0FMATRIX m1) {
    // The inverse of a rotation followed by a translation: the transposed
    // 3x3 and the translation taken back through it.
    sceVu0FMATRIX r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r[i][j] = m1[j][i];
        }
        r[i][3] = 0.0f;
    }
    for (int j = 0; j < 3; j++) {
        r[3][j] = -(r[0][j] * m1[3][0] + r[1][j] * m1[3][1] + r[2][j] * m1[3][2]);
    }
    r[3][3] = m1[3][3];
    std::memcpy(m0, r, sizeof(r));
}

void sceVu0RotMatrixX(sceVu0FMATRIX m0, sceVu0FMATRIX m1, float rx) {
    float         c = std::cos(rx), s = std::sin(rx);
    sceVu0FMATRIX work;
    sceVu0UnitMatrix(work);
    work[1][1] = c;
    work[1][2] = s;
    work[2][1] = -s;
    work[2][2] = c;
    Mul(m0, work, m1);
}

void sceVu0RotMatrixY(sceVu0FMATRIX m0, sceVu0FMATRIX m1, float ry) {
    float         c = std::cos(ry), s = std::sin(ry);
    sceVu0FMATRIX work;
    sceVu0UnitMatrix(work);
    work[0][0] = c;
    work[0][2] = -s;
    work[2][0] = s;
    work[2][2] = c;
    Mul(m0, work, m1);
}

void sceVu0RotMatrixZ(sceVu0FMATRIX m0, sceVu0FMATRIX m1, float rz) {
    float         c = std::cos(rz), s = std::sin(rz);
    sceVu0FMATRIX work;
    sceVu0UnitMatrix(work);
    work[0][0] = c;
    work[0][1] = s;
    work[1][0] = -s;
    work[1][1] = c;
    Mul(m0, work, m1);
}

void sceVu0RotMatrix(sceVu0FMATRIX m0, sceVu0FMATRIX m1, sceVu0FVECTOR rot) {
    float rx = rot[0], ry = rot[1], rz = rot[2];
    sceVu0RotMatrixZ(m0, m1, rz);
    sceVu0RotMatrixY(m0, m0, ry);
    sceVu0RotMatrixX(m0, m0, rx);
}

void sceVu0TransMatrix(sceVu0FMATRIX m0, sceVu0FMATRIX m1, sceVu0FVECTOR tv) {
    float t[3] = {tv[0], tv[1], tv[2]};
    std::memmove(m0, m1, sizeof(sceVu0FMATRIX));
    m0[3][0] += t[0];
    m0[3][1] += t[1];
    m0[3][2] += t[2];
}

void sceVu0CameraMatrix(sceVu0FMATRIX m, sceVu0FVECTOR p, sceVu0FVECTOR zd, sceVu0FVECTOR yd) {
    sceVu0FMATRIX m0;
    sceVu0FVECTOR xd;
    sceVu0UnitMatrix(m0);
    sceVu0OuterProduct(xd, yd, zd);
    sceVu0Normalize(m0[0], xd);
    sceVu0Normalize(m0[2], zd);
    sceVu0OuterProduct(m0[1], m0[2], m0[0]);
    sceVu0TransMatrix(m0, m0, p);
    sceVu0InversMatrix(m, m0);
}

void sceVu0ClampVector(sceVu0FVECTOR v0, sceVu0FVECTOR v1, float min, float max) {
    for (int i = 0; i < 4; i++) {
        float f = v1[i];
        v0[i] = f < min ? min : (f > max ? max : f);
    }
}

void sceVpu0Reset(void) {}

} // extern "C"
