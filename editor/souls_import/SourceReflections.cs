// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// Preserve authored MSB reflection controls independently of material shading.
// ReflectPlaneHeight is a source placement value, not a mesh-derived height.
internal static class SourceReflections {
    // A bounded asset-reader guard, not a claimed native rendering limit.
    const float MaxCoordinate=1000000;
    internal static void CopyFlags(MSB1.Part source,MSBB.Part target) {
        ArgumentNullException.ThrowIfNull(source);ArgumentNullException.ThrowIfNull(target);
        if(source.DrawByReflectCam>1||source.DrawOnlyReflectCam>1)
            throw new InvalidDataException("Invalid source reflection flags: "+source.Name);
        target.DrawByReflectCam=source.DrawByReflectCam==1;
        target.DrawOnlyReflectCam=source.DrawOnlyReflectCam==1;
    }
    internal static void CopyPlane(MSB1.Part.Collision source,MSBB.Part.Collision target,float offsetY) {
        ArgumentNullException.ThrowIfNull(source);ArgumentNullException.ThrowIfNull(target);
        float height=source.ReflectPlaneHeight+offsetY;
        if(!float.IsFinite(source.ReflectPlaneHeight)||Math.Abs(source.ReflectPlaneHeight)>MaxCoordinate||
            !float.IsFinite(offsetY)||Math.Abs(offsetY)>MaxCoordinate||!float.IsFinite(height)||Math.Abs(height)>MaxCoordinate)
            throw new InvalidDataException("Invalid source reflection plane: "+source.Name);
        target.ReflectPlaneHeight=height;
    }
}
