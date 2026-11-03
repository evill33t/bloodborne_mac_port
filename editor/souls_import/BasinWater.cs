// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Buffers.Binary;

// The caller scopes this adapter to DS1R m12. Water is render geometry, never
// a replacement for the source high/low collision or the lake's death volume.
internal sealed class BasinWater {
    readonly Func<string,MTD> sourceDefinition;
    readonly FLVER2 donor;
    readonly FLVER2.Material prototype;
    readonly Dictionary<string,EnvironmentMaterial> definitions=new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<string,TPF.Texture> nativeTextures=new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<string,string> nativeTextureNames=new(StringComparer.OrdinalIgnoreCase);
    readonly Func<string,MTD> nativeDefinition;
    internal IEnumerable<EnvironmentMaterial> Definitions=>definitions.Values;
    // Already in native PS4 format: merge directly into the output texture bank,
    // without passing through source DDS, sRGB or normal-map conversion.
    internal IReadOnlyDictionary<string,TPF.Texture> NativeTextures=>nativeTextures;
    internal int Surfaces {get;private set;}
    internal static readonly string[] Limitations=[
        "Source reflective-water geometry, rigid transforms, normal texture and three scrolling wave layers use a native BB water shader. Source FRPG Fresnel curve, wave amplitude, depth absorption and planar-reflection rendering are approximations requiring gameplay comparison.",
        "Source shoreline geometry is retained, but the native profile's constant vertex control colour replaces source vertex alpha; equivalence between those shader controls is unverified. Native lake diffuse/specular/shininess pixels, Fresnel endpoints, material controls and depth response are retained. Source absorption colour, specular power and reflection/refraction bands have no verified native conversion and are not used as diffuse paint. Source Fresnel bias/scale/power are not equivalent to the native endpoints; the native lake baseline avoids the overbright surface observed when copying source bias.",
        "Waterfall/particle shaders and source water interaction SFX are separate from reflective lake/river surfaces and are not converted by this adapter."];
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    internal BasinWater(Func<string,MTD> sourceDefinition,Func<string,byte[]?> nativeRead,Func<string,MTD> nativeDefinition) {
        this.sourceDefinition=sourceDefinition;this.nativeDefinition=nativeDefinition;
        donor=FLVER2.Read(nativeRead("map/m32_00_00_00/m32_00_00_00_005070.flver.dcx")??throw new FileNotFoundException("Native lake water profile unavailable"));
        prototype=donor.Materials.Single(m=>Leaf(m.MTD)=="M[ARSN]_water.mtd");
        var definition=nativeDefinition(prototype.MTD);
        var gx=donor.GXLists[prototype.GXIndex].Single(g=>g.ID=="GX07");
        var surface=donor.Meshes.Single(m=>m.MaterialIndex==donor.Materials.IndexOf(prototype));
        var layout=surface.VertexBuffers.SelectMany(b=>donor.BufferLayouts[b.LayoutIndex]).ToArray();
        (uint Type,uint Semantic,int Index)[] expected=[(2,0,0),(17,3,0),(17,6,0),(17,6,1),(17,6,2),(19,10,1),(22,5,0),(21,5,1)];
        string[] slots=["g_DiffuseTexture","g_DiffuseTexture2","g_SpecularTexture","g_SpecularTexture2","g_ShininessTexture","g_ShininessTexture2","g_BumpmapTexture","g_BumpmapTexture2","g_BumpmapTexture3"];
        if(Leaf(definition.ShaderPath)!="GXFlver_WaterBumpTri.spx"||gx.Data.Length!=116||gx.Unk04!=104||surface.UseBoneWeights||
            !layout.Select(m=>((uint)m.Type,(uint)m.Semantic,m.Index)).SequenceEqual(expected)||layout.Any(m=>m.Stream!=0||m.SpecialModifier!=0)||
            !prototype.Textures.Select(t=>t.ParamName).Order(StringComparer.Ordinal).SequenceEqual(slots.Order(StringComparer.Ordinal)))
            throw new InvalidDataException("Native water shader profile changed");
        using var textures=new BXF4Reader(nativeRead("map/m32/m32_0003.tpfbhd")??throw new FileNotFoundException("Native lake texture header unavailable"),
            nativeRead("map/m32/m32_0003.tpfbdt")??throw new FileNotFoundException("Native lake texture data unavailable"));
        foreach(var slot in prototype.Textures.Where(t=>!t.ParamName.Contains("Bump",StringComparison.Ordinal))) {
            string name=Path.GetFileNameWithoutExtension(Leaf(slot.Path));
            if(!new[]{"m32_00_water_853_a","m32_00_water_854_a","m32_00_water_853_r","m32_00_water_854_r","m32_00_water_853_s","m32_00_water_854_s"}.Contains(name,StringComparer.Ordinal))
                throw new InvalidDataException("Unverified native water texture dependency "+name);
            var member=textures.Files.Single(f=>Leaf(f.Name)==name+".tpf.dcx");var tpf=TPF.Read(textures.ReadFile(member));
            if(tpf.Platform!=TPF.TPFPlatform.PS4||tpf.Encoding!=1||tpf.Flag2!=3||tpf.Textures.Count!=1||tpf.Textures[0].Name!=name||tpf.Textures[0].Type!=TPF.TexType.Texture)
                throw new InvalidDataException("Native water texture record changed "+name);
            // Parse a separate record so renaming cannot mutate the reader's
            // pixels or header. No decompression/recompression of texture data.
            var image=TPF.Read(tpf.Write()).Textures.Single();image.Name="BBImport_m12_water_native_"+name;
            nativeTextures.Add(image.Name,image);nativeTextureNames.Add(name,image.Name);
        }
        if(nativeTextures.Count!=6)throw new InvalidDataException("Incomplete native water texture closure");
    }
    internal bool IsSurface(IFlverMaterial source) {
        // Character materials have duplicate retail names and unrelated shader
        // contracts. Do not look them up as map surface definitions.
        if(Leaf(source.MTD).StartsWith("P_",StringComparison.OrdinalIgnoreCase))return false;
        return Leaf(sourceDefinition(source.MTD).ShaderPath)=="FRPG_Water_Reflect.spx";
    }
    static float Scalar(MTD d,string name) {
        float v=(float)d.Params.Single(p=>p.Name==name).Value;
        if(!float.IsFinite(v)||Math.Abs(v)>1024)throw new InvalidDataException("Invalid source water scalar "+name);return v;
    }
    static float[] Array(MTD d,string name,int size) {
        var v=(float[])d.Params.Single(p=>p.Name==name).Value;
        if(v.Length!=size||v.Any(f=>!float.IsFinite(f)||Math.Abs(f)>1024))throw new InvalidDataException("Invalid source water vector "+name);return v;
    }
    static Vector2 Tile(MTD d,int layer) {
        string name="g_TileScale_"+layer;var value=d.Params.Single(p=>p.Name==name).Value;
        if(value is float)return new(Scalar(d,name));
        var pair=Array(d,name,2);return new(pair[0],pair[1]);
    }
    internal (FLVER2.Mesh Mesh,FLVER2.Material Material) Convert(IFlverMaterial source,IFlverMesh mesh,StaticMapGeometry geometry,FLVER2 model,string area,
        Func<string,string,string> texture) {
        if(!IsSurface(source))throw new InvalidDataException("Unsupported water shader");
        if(mesh.Vertices.Count is <3 or >1000000||mesh.Vertices.Any(v=>v.UVs.Count==0||!float.IsFinite(v.UVs[0].X)||!float.IsFinite(v.UVs[0].Y)))
            throw new InvalidDataException("Reflective water requires bounded source UV geometry");
        var from=sourceDefinition(source.MTD);
        if((int)from.Params.Single(p=>p.Name=="g_TexScrollType").Value is not(0 or 1))throw new InvalidDataException("Unsupported source water scroll mode");
        if(!definitions.TryGetValue(Leaf(source.MTD),out var owned)) {
            var mtd=MTD.Read(nativeDefinition(prototype.MTD).Write());
            // Keep the actual native lake response. The source water colour is
            // absorption, not a diffuse tint, and source specular power does
            // not establish the native roughness/reflectance contract.
            owned=new("BBImport_m12_water_"+definitions.Count.ToString("D3")+".mtd",mtd);definitions.Add(Leaf(source.MTD),owned);
        }
        var material=new FLVER2.Material("Imported_water_"+model.Materials.Count,@"N:\SPRJ\data\Material\mtd\map\BBImport\"+owned.Name,prototype.Index);
        var nativeList=donor.GXLists[prototype.GXIndex];var list=new FLVER2.GXList{TerminatorID=nativeList.TerminatorID,TerminatorLength=nativeList.TerminatorLength};
        foreach(var item in nativeList)list.Add(new(item.ID,item.Unk04,item.Data.ToArray()));
        var gx=list.Single(g=>g.ID=="GX07");
        void Set(int index,float value)=>BinaryPrimitives.WriteSingleLittleEndian(gx.Data.AsSpan(index*4,4),value);
        for(int i=0;i<3;i++) {
            var scroll=Array(from,"g_TexScroll_"+i,2);bool moving=(int)from.Params.Single(p=>p.Name=="g_TexScrollType").Value==1;
            int index=i==2?20:i*2;Set(index,moving?scroll[0]:0);Set(index+1,moving?scroll[1]:0);
        }
        // Source bias/scale/power do not establish native Fresnel endpoints.
        // Keep the shipped lake response, including reflection strength, depth,
        // refraction and fade. Copying source bias produced a white surface in
        // native gameplay; changing global exposure would hide that mismatch.
        material.GXIndex=model.GXLists.Count;model.GXLists.Add(list);
        // UV multiplication stays in per-slot shader constants, avoiding packed
        // short overflow. Rescale UVs and constants together when needed.
        float extent=mesh.Vertices.SelectMany(v=>v.UVs.Take(1)).Select(v=>Math.Max(Math.Abs(v.X),Math.Abs(v.Y))).DefaultIfEmpty().Max();
        float factor=1;while(extent*factor>15)factor*=.5f;
        string bump=source.Textures.Single(t=>t.ParamName=="g_Bumpmap").Path;
        foreach(var slot in prototype.Textures) {
            int layer=slot.ParamName.EndsWith("3",StringComparison.Ordinal)?2:slot.ParamName.EndsWith("2",StringComparison.Ordinal)?1:0;
            bool normal=slot.ParamName.Contains("Bump",StringComparison.Ordinal);
            var tiling=normal?Tile(from,layer):slot.TilingScale;
            string name=normal?texture(bump,"normal"):nativeTextureNames[Path.GetFileNameWithoutExtension(Leaf(slot.Path))];
            string path=@"N:\SPRJ\data\Model\map\"+area+@"\tex\"+name+".tif";
            material.Textures.Add(new(slot.ParamName,path,tiling/factor,slot.TilingTypeU,slot.TilingTypeV,slot.Unk14,slot.Unk18,slot.Unk1C));
        }
        var nativeMesh=donor.Meshes.First(m=>m.MaterialIndex==donor.Materials.IndexOf(prototype));
        var output=new FLVER2.Mesh{UseBoneWeights=false,NodeIndex=0,MaterialIndex=model.Materials.Count};
        foreach(var buffer in nativeMesh.VertexBuffers) {
            var layout=new FLVER2.BufferLayout();foreach(var member in donor.BufferLayouts[buffer.LayoutIndex])layout.Add(new FLVER.LayoutMember(member));
            int index=model.BufferLayouts.Count;model.BufferLayouts.Add(layout);output.VertexBuffers.Add(new(index){BufferIndex=buffer.BufferIndex});
        }
        foreach(var vertex in mesh.Vertices) {
            var v=geometry.Bake(mesh,vertex,true);var uv=vertex.UVs.Count==0?Vector3.Zero:vertex.UVs[0]*factor;
            if(!float.IsFinite(uv.X)||!float.IsFinite(uv.Y))throw new InvalidDataException("Invalid source water UV");
            v.UVs.Clear();for(int i=0;i<3;i++)v.UVs.Add(uv);
            // All three normal layers use source UV0, so they share its basis.
            // StaticMapGeometry's generic UV1 filler is unrelated to this UV
            // and must not invert the second wave layer's handedness.
            var tangent=v.Tangents[0];v.Tangents.Clear();for(int i=0;i<3;i++)v.Tangents.Add(tangent);
            // Keep the actual shipped native water control value. Its shader
            // meaning is not proven equivalent to source shore-fade alpha.
            v.Colors.Clear();v.Colors.Add(new FLVER.VertexColor(1f,1f,0f,0f));output.Vertices.Add(v);
        }
        Surfaces++;return(output,material);
    }
}
