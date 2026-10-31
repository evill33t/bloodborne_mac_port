// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// Used only for converted static map meshes, after their native material exists.
// Keep the coordinate and texture-scale changes together; actor/sky materials
// have separate shader contracts and do not pass through this adapter.
internal static class StaticMapUV {
    internal static void Apply(FLVER2.Mesh mesh,FLVER2.Material material,string profile) {
        float scale=MapMaterial.UVScale(profile);if(scale==1)return;
        bool overflow=false;
        foreach(var vertex in mesh.Vertices)foreach(var uv in vertex.UVs) {
            if(!float.IsFinite(uv.X)||!float.IsFinite(uv.Y)||!float.IsFinite(uv.Z))throw new InvalidDataException("Invalid static UV");
            overflow|=uv.X<short.MinValue/2048f||uv.X>short.MaxValue/2048f||uv.Y<short.MinValue/2048f||uv.Y>short.MaxValue/2048f;
        }
        if(!overflow)return;
        string shader=Path.GetFileName(material.MTD.Replace('\\','/'));
        if(!shader.Equals("M[ARSN].mtd",StringComparison.OrdinalIgnoreCase)&&!shader.Equals("M[ARSN]_e.mtd",StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Overflowing static UV requires the supported native map material: "+material.MTD);
        var tiling=material.Textures.Select(t=>MapMaterial.NativeTilingScale(t.TilingScale,profile)).ToArray();
        if(tiling.Any(t=>!float.IsFinite(t.X)||!float.IsFinite(t.Y)))throw new InvalidDataException("Invalid static texture tiling scale");
        foreach(var vertex in mesh.Vertices)foreach(var uv in vertex.UVs) {
            if(uv.X*scale<short.MinValue/2048f||uv.X*scale>short.MaxValue/2048f||uv.Y*scale<short.MinValue/2048f||uv.Y*scale>short.MaxValue/2048f)
                throw new InvalidDataException("Source static UV exceeds the native packed range");
        }
        foreach(var vertex in mesh.Vertices)for(int i=0;i<vertex.UVs.Count;++i)vertex.UVs[i]*=scale;
        for(int i=0;i<material.Textures.Count;++i)material.Textures[i].TilingScale=tiling[i];
    }
}
