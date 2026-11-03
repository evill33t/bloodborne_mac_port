// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Numerics;

// The arena has its own authored GI capture. Transfer its specular cube
// without applying a guessed diffuse-GI gain or changing other rooms.
internal static class PinwheelEnvironment {
    internal const string TexturePath="map/m40/m40_envmap.tpf.dcx",TextureName="m40_05_GILM0501";
    internal const int TextureID=501;
    internal sealed record Report(IReadOnlyList<string> Parts,int EnvironmentRow,string SourceCapture,string SourceCube,int Faces,int Mips,int Width,int Height,
        IReadOnlyDictionary<int,IReadOnlyDictionary<string,object>> SourceEnvironmentControls,IReadOnlyList<string> Limitations);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,Report Report);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool condition,string reason){if(!condition)throw new InvalidDataException("Pinwheel environment: "+reason);}
    static void Append<T>(GPARAM.Field<T> field,int id){Need(field.Values.Count<255&&!field.Values.Any(v=>v.ID==id),"GPARAM capacity/row collision");var from=field.Values.Single(v=>v.ID==0);field.Values.Add(new(){ID=id,TimeOfDay=from.TimeOfDay,Value=from.Value});field.Capacity=checked((short)field.Values.Count);}
    static GPARAM.Param Clone(GPARAM g,int id) {
        var group=g.Params.Single(p=>p.Key=="Env Map:Editor");
        foreach(var field in group.Fields)switch(field) {
            case GPARAM.ShortField f:Append(f,id);break;case GPARAM.IntField f:Append(f,id);break;case GPARAM.UintField f:Append(f,id);break;
            case GPARAM.FloatField f:Append(f,id);break;case GPARAM.BoolField f:Append(f,id);break;case GPARAM.Vector2Field f:Append(f,id);break;
            case GPARAM.Vector3Field f:Append(f,id);break;case GPARAM.Vector4Field f:Append(f,id);break;case GPARAM.ColorField f:Append(f,id);break;
            default:throw new InvalidDataException("Unsupported arena GPARAM field: "+field.GetType().Name);
        }
        int index=g.Params.IndexOf(group);var extra=g.UnkParamExtras.SingleOrDefault(e=>e.GroupIndex==index);
        if(extra==null){extra=new(){GroupIndex=index};int at=g.UnkParamExtras.FindIndex(e=>e.GroupIndex>index);if(at<0)g.UnkParamExtras.Add(extra);else g.UnkParamExtras.Insert(at,extra);}
        Need(!extra.Ids.Contains(id),"environment index already owns row");extra.Ids.Add(id);return group;
    }
    internal static Result Apply(EnvironmentResult environment,Func<string,byte[]?> sourceRead,Func<string,byte[]?> composedRead,string definitionsDirectory,MSB1 sourceMap) {
        Need(!environment.Rows.Any(r=>r.Kind=="PinwheelReflection"),"arena reflection already applied");
        var floor=sourceMap.Parts.Collisions.Single(p=>p.Name=="h0036B0");
        Need(floor.EnvLightMapSpotName=="【環境ポイント】ボス部屋","source arena capture ownership changed");
        var capture=sourceMap.Events.Environments.Single(e=>e.Name==floor.EnvLightMapSpotName);
        Need(capture.EntityID==1,"source arena capture ID changed");
        var actors=sourceMap.Parts.Enemies.Where(p=>p.ModelName=="c3320").ToArray();
        Need(actors.Length==15&&actors.Select(p=>p.EntityID).Order().SequenceEqual(Enumerable.Range(1300800,15))&&actors.All(p=>p.CollisionName==floor.Name),"source boss/helper arena ownership changed");
        string[] meshNames=["m2500B0","m2501B0","m2502B0"];
        Need(meshNames.All(n=>sourceMap.Parts.MapPieces.Any(p=>p.Name==n&&p.LightID==3)),"source arena scenery family changed");
        var parts=sourceMap.Parts.GetEntries().Where(p=>p.Name==floor.Name||p is MSB1.Part.MapPiece&&meshNames.Contains(p.Name,StringComparer.Ordinal)||
            p is MSB1.Part.Enemy e&&e.CollisionName==floor.Name||p is MSB1.Part.Object o&&o.CollisionName==floor.Name).ToArray();
        Need(parts.All(p=>environment.Bindings.ContainsKey(p.Name)),"source arena environment binding unavailable");
        byte[] Source(string p)=>sourceRead(p)??throw new FileNotFoundException("Source Pinwheel environment unavailable",p);
        var split=BXF3.Read(Source("map/m13/GI_EnvM_m13.tpfbhd"),Source("map/m13/GI_EnvM_m13.tpfbdt"));
        const string captureName="GI_Env_m13_00_0001.tpf.dcx",cubeName="GI_EnvSpc_m13_00_0001_0";
        var captureBank=TPF.Read(split.Files.Single(f=>Leaf(f.Name)==captureName).Bytes.ToArray());
        Need(captureBank.Textures.Count==2&&captureBank.Textures.Any(t=>t.Name=="GI_EnvDif_m13_00_0001"),"source arena diffuse/specular capture family changed");
        var cube=captureBank.Textures.Single(t=>t.Name==cubeName);var sourceDds=cube.HeaderizeExt(out var ext);var sourceHeader=new DDS(sourceDds);
        Need(ext==".dds"&&cube.Type==TPF.TexType.Cubemap&&sourceHeader.dwWidth==256&&sourceHeader.dwHeight==256&&sourceHeader.dwMipMapCount==8&&(int)sourceHeader.GetDXGIFormat()==95,"source arena BC6H cube contract changed");
        var textures=TPF.Read(composedRead(TexturePath)??throw new FileNotFoundException("Pinwheel environment requires the staged shared m40 cube bank",TexturePath));
        Need(textures.Platform==TPF.TPFPlatform.PS4&&textures.Encoding==1&&textures.Flag2==3&&textures.Textures.Select(t=>t.Name).Distinct(StringComparer.Ordinal).Count()==textures.Textures.Count&&!textures.Textures.Any(t=>t.Name.Equals(TextureName,StringComparison.OrdinalIgnoreCase)),"native cube bank metadata/owned texture collision");
        var nativeCube=BasinEnvironment.Cube(TextureName,cube,textures.Textures.First(t=>t.Type==TPF.TexType.Cubemap&&t.Format==100));textures.Textures.Add(nativeCube);
        var profiles=environment.Files.Where(f=>f.Path.EndsWith(".gparam.dcx",StringComparison.Ordinal)).Select(f=>GPARAM.Read(f.Bytes)).ToArray();Need(profiles.Length>0,"native environment profiles unavailable");
        var used=profiles.SelectMany(g=>g.Params.SelectMany(p=>p.Fields).SelectMany(f=>f.Values).Select(v=>v.ID).Concat(g.UnkParamExtras.SelectMany(e=>e.Ids))).ToHashSet();int row=35000;while(used.Contains(row))row++;
        byte[] Patch(byte[] bytes){var g=GPARAM.Read(bytes);Need(g.Version==GPARAM.GparamVersion.V3,"native GPARAM V3 required");var group=Clone(g,row);
            ((GPARAM.UintField)group.Fields.Single(f=>f.Key=="Env Map: EnvMapId")).Values.Single(v=>v.ID==row).Value=TextureID;
            ((GPARAM.FloatField)group.Fields.Single(f=>f.Key=="Env Map: ColorScale")).Values.Single(v=>v.ID==row).Value=1f;
            return g.Write(DCX.Type.DCX_DFLT_10000_44_9);}
        var patched=new List<EnvironmentFile>();foreach(var file in environment.Files) {
            if(file.Path.EndsWith(".gparambnd.dcx",StringComparison.Ordinal)){var bank=BND4.Read(file.Bytes);foreach(var f in bank.Files)f.Bytes=DCX.Decompress(Patch(f.Bytes.ToArray()));patched.Add(file with{Bytes=bank.Write(DCX.Type.DCX_DFLT_10000_44_9)});}
            else if(file.Path.EndsWith(".gparam.dcx",StringComparison.Ordinal))patched.Add(file with{Bytes=Patch(file.Bytes)});
            else throw new InvalidDataException("Unexpected Pinwheel environment output: "+file.Path);
        }
        var draw=BND3.Read(Source("param/DrawParam/a13_DrawParam.parambnd.dcx"));var light=PARAM.Read(draw.Files.Single(f=>Leaf(f.Name)=="m13_LightBank.param").Bytes);
        light.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitionsDirectory,"LightBank.xml")));
        var controls=parts.Select(p=>(int)p.LightID).Distinct().Order().ToDictionary(id=>id,id=>(IReadOnlyDictionary<string,object>)light.Rows.Single(r=>r.ID==id).Cells.Where(c=>c.Def.InternalName.StartsWith("env",StringComparison.Ordinal)).ToDictionary(c=>c.Def.InternalName,c=>c.Value));
        string[] limits=["Only source arena h0036B0, m2500B0/m2501B0/m2502B0 and the actual enemy/object placements bound to that floor receive the private authored specular capture. Other Catacombs rooms and shared map cubes are unchanged.",
            "The exact source 256×256 BC6H specular cube, six faces and eight mips are retained at native unit gain. Native orientation and material response require a matched gameplay comparison; separate source diffuse GI and independent envDif/envSpc tint/intensity remain unconverted.",
            "DS1 g_Lightmap textures and their lighting-type3 shader remain unconverted. Source single lightmaps cannot be assumed equivalent to native two-texture RNM DOL materials. Direct lights, fog, tone, source visibility and authored arena geometry are unchanged; this is not a claim of matched warm room lighting."];
        var textureBytes=textures.Write(DCX.Type.DCX_DFLT_10000_44_9);
        // Commit after source, profile and texture conversion have all passed.
        environment.Files.Clear();environment.Files.AddRange(patched);foreach(var p in parts){var binding=environment.Bindings[p.Name].DeepCopy();binding.EnvMapID=row;environment.Bindings[p.Name]=binding;}
        environment.Rows.Add(new("PinwheelReflection",1,row,new(){["sourceCapture"]=captureName,["sourceCube"]=cubeName,["nativeTexture"]=TextureName,["nativeGain"]=1f},"Lossless authored arena BC6H specular capture; source diffuse GI and baked lightmaps remain unconverted."));environment.Limitations.AddRange(limits);
        return new(new Dictionary<string,byte[]>{{TexturePath,textureBytes}},new(parts.Select(p=>p.Name).Order(StringComparer.Ordinal).ToArray(),row,captureName,cubeName,6,8,256,256,controls,limits));
    }
}
