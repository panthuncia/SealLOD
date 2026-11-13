using System;
using System.Globalization;
using System.Numerics;
using System.Runtime.InteropServices;

namespace BasicRenderer.Scripting;

/// <summary>
/// A position, direction or scale in metres. Right-handed, +Y up; cameras look along -Z, lights shine along +Z.
/// Layout-identical to the ABI's BRVec3. Converts from tuples: <c>(0, 1.5, -2)</c>.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public readonly struct Vec3 : IEquatable<Vec3>
{
    public readonly float X, Y, Z;

    public Vec3(float x, float y, float z)
    {
        X = x;
        Y = y;
        Z = z;
    }

    public static readonly Vec3 Zero = new(0, 0, 0);
    public static readonly Vec3 One = new(1, 1, 1);
    public static readonly Vec3 UnitX = new(1, 0, 0);
    public static readonly Vec3 UnitY = new(0, 1, 0);
    public static readonly Vec3 UnitZ = new(0, 0, 1);
    public static readonly Vec3 Up = UnitY;
    /// <summary>A camera's view direction before rotation (-Z).</summary>
    public static readonly Vec3 Forward = new(0, 0, -1);

    public float Length => MathF.Sqrt(X * X + Y * Y + Z * Z);
    public float LengthSquared => X * X + Y * Y + Z * Z;

    public Vec3 Normalized
    {
        get
        {
            float length = Length;
            return length > 1e-12f ? this / length : Zero;
        }
    }

    public static float Dot(Vec3 a, Vec3 b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    public static Vec3 Cross(Vec3 a, Vec3 b) => new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    public static float Distance(Vec3 a, Vec3 b) => (a - b).Length;
    public static Vec3 Lerp(Vec3 a, Vec3 b, float t) => a + (b - a) * t;

    public static Vec3 operator +(Vec3 a, Vec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vec3 operator -(Vec3 a, Vec3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vec3 operator -(Vec3 a) => new(-a.X, -a.Y, -a.Z);
    public static Vec3 operator *(Vec3 a, float s) => new(a.X * s, a.Y * s, a.Z * s);
    public static Vec3 operator *(float s, Vec3 a) => a * s;
    public static Vec3 operator *(Vec3 a, Vec3 b) => new(a.X * b.X, a.Y * b.Y, a.Z * b.Z);
    public static Vec3 operator /(Vec3 a, float s) => new(a.X / s, a.Y / s, a.Z / s);
    public static bool operator ==(Vec3 a, Vec3 b) => a.Equals(b);
    public static bool operator !=(Vec3 a, Vec3 b) => !a.Equals(b);

    public static implicit operator Vec3((float X, float Y, float Z) t) => new(t.X, t.Y, t.Z);
    public static implicit operator Vec3((double X, double Y, double Z) t) => new((float)t.X, (float)t.Y, (float)t.Z);
    public static implicit operator Vec3(Vector3 v) => new(v.X, v.Y, v.Z);
    public static implicit operator Vector3(Vec3 v) => new(v.X, v.Y, v.Z);

    /// <summary>True when every component is within <paramref name="tolerance"/> of the other's.</summary>
    public bool ApproximatelyEquals(Vec3 other, float tolerance = 1e-4f) =>
        MathF.Abs(X - other.X) <= tolerance && MathF.Abs(Y - other.Y) <= tolerance && MathF.Abs(Z - other.Z) <= tolerance;

    public bool Equals(Vec3 other) => X == other.X && Y == other.Y && Z == other.Z;
    public override bool Equals(object? obj) => obj is Vec3 other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(X, Y, Z);
    public override string ToString() => string.Create(CultureInfo.InvariantCulture, $"({X:0.###}, {Y:0.###}, {Z:0.###})");
}

/// <summary>A rotation (unit quaternion). Layout-identical to the ABI's BRQuat. Angles in the API are degrees.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly struct Quat : IEquatable<Quat>
{
    public readonly float X, Y, Z, W;

    public Quat(float x, float y, float z, float w)
    {
        X = x;
        Y = y;
        Z = z;
        W = w;
    }

    public static readonly Quat Identity = new(0, 0, 0, 1);

    /// <summary>A rotation of <paramref name="degrees"/> about <paramref name="axis"/> (right-handed: counter-clockwise looking down the axis).</summary>
    public static Quat AxisAngle(Vec3 axis, double degrees) => Quaternion.CreateFromAxisAngle(((Vector3)axis.Normalized), (float)(degrees * Math.PI / 180.0));

    /// <summary>Yaw about +Y, then pitch about +X, then roll about +Z, in degrees.</summary>
    public static Quat Euler(double pitchDegrees, double yawDegrees, double rollDegrees = 0) =>
        Quaternion.CreateFromYawPitchRoll((float)(yawDegrees * Math.PI / 180.0), (float)(pitchDegrees * Math.PI / 180.0), (float)(rollDegrees * Math.PI / 180.0));

    /// <summary>The rotation that turns <paramref name="from"/> onto <paramref name="to"/> by the shortest arc.</summary>
    public static Quat FromTo(Vec3 from, Vec3 to)
    {
        Vec3 a = from.Normalized, b = to.Normalized;
        float dot = Vec3.Dot(a, b);
        if (dot > 0.999999f) return Identity;
        if (dot < -0.999999f) {
            // Opposite: any axis perpendicular to a.
            Vec3 axis = Vec3.Cross(Vec3.UnitX, a);
            if (axis.LengthSquared < 1e-6f) axis = Vec3.Cross(Vec3.UnitY, a);
            return AxisAngle(axis, 180);
        }
        Vec3 c = Vec3.Cross(a, b);
        return new Quat(c.X, c.Y, c.Z, 1 + dot).Normalized;
    }

    /// <summary>
    /// The camera orientation looking along <paramref name="direction"/> with <paramref name="up"/> as up: what the
    /// renderer's XMMatrixLookAtRH view is the inverse of. Local -Z maps to direction.
    /// </summary>
    public static Quat LookRotation(Vec3 direction, Vec3? up = null)
    {
        Vec3 forward = direction.Normalized;
        if (forward.LengthSquared < 1e-12f) return Identity;
        Vec3 u = up ?? Vec3.Up;
        if (MathF.Abs(Vec3.Dot(forward, u.Normalized)) > 0.9999f) u = MathF.Abs(forward.Y) < 0.9f ? Vec3.Up : Vec3.UnitZ;
        // Columns of the camera's model matrix: right, up, back (+Z points away from the view direction).
        Vec3 back = -forward;
        Vec3 right = Vec3.Cross(u, back).Normalized;
        Vec3 trueUp = Vec3.Cross(back, right);
        var basis = new Matrix4x4(
            right.X, right.Y, right.Z, 0,
            trueUp.X, trueUp.Y, trueUp.Z, 0,
            back.X, back.Y, back.Z, 0,
            0, 0, 0, 1);
        return Quaternion.Normalize(Quaternion.CreateFromRotationMatrix(basis));
    }

    /// <summary>The rotation that points local +Z along <paramref name="direction"/>: a light's orientation.</summary>
    public static Quat PointZ(Vec3 direction, Vec3? up = null) => LookRotation(-direction, up);

    public Quat Normalized => Quaternion.Normalize(this);
    public Quat Inverse => Quaternion.Inverse(this);

    public static Quat Slerp(Quat a, Quat b, float t) => Quaternion.Slerp(a, b, t);

    /// <summary>This rotation applied to a vector.</summary>
    public Vec3 Rotate(Vec3 v) => Vector3.Transform((Vector3)v, (Quaternion)this);

    /// <summary>a then b: rotating by the result is rotating by a, then by b.</summary>
    public static Quat operator *(Quat a, Quat b) => Quaternion.Concatenate(a, b);
    public static Vec3 operator *(Quat q, Vec3 v) => q.Rotate(v);
    public static bool operator ==(Quat a, Quat b) => a.Equals(b);
    public static bool operator !=(Quat a, Quat b) => !a.Equals(b);

    public static implicit operator Quat(Quaternion q) => new(q.X, q.Y, q.Z, q.W);
    public static implicit operator Quaternion(Quat q) => new(q.X, q.Y, q.Z, q.W);

    /// <summary>The angle between two rotations in degrees, ignoring the sign ambiguity of quaternions.</summary>
    public static double AngleBetween(Quat a, Quat b)
    {
        double dot = Math.Abs(a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W);
        return 2.0 * Math.Acos(Math.Min(1.0, dot)) * 180.0 / Math.PI;
    }

    public bool Equals(Quat other) => X == other.X && Y == other.Y && Z == other.Z && W == other.W;
    public override bool Equals(object? obj) => obj is Quat other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(X, Y, Z, W);
    public override string ToString() => string.Create(CultureInfo.InvariantCulture, $"({X:0.####}, {Y:0.####}, {Z:0.####}, {W:0.####})");
}

/// <summary>Position, rotation and scale. Layout-identical to the ABI's BRTransform.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly struct Transform
{
    public readonly Vec3 Position;
    public readonly Quat Rotation;
    public readonly Vec3 Scale;

    public Transform(Vec3 position, Quat rotation, Vec3 scale)
    {
        Position = position;
        Rotation = rotation;
        Scale = scale;
    }

    public static readonly Transform Identity = new(Vec3.Zero, Quat.Identity, Vec3.One);

    public Transform WithPosition(Vec3 position) => new(position, Rotation, Scale);
    public Transform WithRotation(Quat rotation) => new(Position, rotation, Scale);
    public Transform WithScale(Vec3 scale) => new(Position, Rotation, scale);

    public static Transform At(Vec3 position) => new(position, Quat.Identity, Vec3.One);

    public override string ToString() => $"position {Position}, rotation {Rotation}, scale {Scale}";
}
