// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Globalization;
using System.Numerics;

// Darkroot's lake uses source capture1. Keep the shared Garden Light/Fog0
// rows untouched: private Basin rows and texture names own this correction.
internal static class BasinEnvironment {
    internal const string TexturePath="map/m40/m40_envmap.tpf.dcx",TextureName="m40_04_GILM0401";
    internal const int Capture=1,TextureID=401;
    internal sealed record Report(IReadOnlyList<string> Parts,int EnvironmentRow,IReadOnlyDictionary<int,int> FogRows,string SourceCapture,string SourceCube,
        int Faces,int Mips,int Width,int Height,IReadOnlyDictionary<string,object> SourceEnvironmentControls,IReadOnlyList<string> Limitations);
    internal sealed record Result(IReadOnlyDictionary<string,byte[]> Files,Report Report);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool condition,string reason){if(!condition)throw new InvalidDataException("Basin environment: "+reason);}
    static float Number(PARAM.Row r,string key){float v=Convert.ToSingle(r[key].Value,CultureInfo.InvariantCulture);Need(float.IsFinite(v),"nonfinite source field "+key);return v;}
    static void Append<T>(GPARAM.Field<T> field,int from,int to){Need(field.Values.Count<255&&!field.Values.Any(v=>v.ID==to),"GPARAM capacity/row collision");var old=field.Values.Single(v=>v.ID==from);field.Values.Add(new(){ID=to,TimeOfDay=old.TimeOfDay,Value=old.Value});field.Capacity=checked((short)field.Values.Count);}
    static GPARAM.Param Clone(GPARAM g,string key,int from,int to) {
        var group=g.Params.Single(p=>p.Key==key);foreach(var field in group.Fields)switch(field) {
            case GPARAM.ShortField f:Append(f,from,to);break;case GPARAM.IntField f:Append(f,from,to);break;case GPARAM.UintField f:Append(f,from,to);break;
            case GPARAM.FloatField f:Append(f,from,to);break;case GPARAM.BoolField f:Append(f,from,to);break;case GPARAM.Vector2Field f:Append(f,from,to);break;
            case GPARAM.Vector3Field f:Append(f,from,to);break;case GPARAM.Vector4Field f:Append(f,from,to);break;case GPARAM.ColorField f:Append(f,from,to);break;
            default:throw new InvalidDataException("Unsupported Basin GPARAM field "+field.GetType().Name);
        }
        int index=g.Params.IndexOf(group);var extra=g.UnkParamExtras.SingleOrDefault(e=>e.GroupIndex==index);
        if(extra==null){extra=new(){GroupIndex=index};int at=g.UnkParamExtras.FindIndex(e=>e.GroupIndex>index);if(at<0)g.UnkParamExtras.Add(extra);else g.UnkParamExtras.Insert(at,extra);}
        Need(!extra.Ids.Contains(to),"GPARAM index already owns row");extra.Ids.Add(to);return group;
    }
    static void Set<T>(GPARAM.Param group,string key,int id,T value)=>((GPARAM.Field<T>)group.Fields.Single(f=>f.Key==key)).Values.Single(v=>v.ID==id).Value=value;
    // Square BC6H DDS cubes are copied face/mip exactly; no HDR decoding,
    // quantization, guessed gamma, channel tint or cubemap rotation is applied.
    internal static TPF.Texture Cube(string name,TPF.Texture source,TPF.Texture native) {
        Need(source.Type==TPF.TexType.Cubemap&&native.Type==TPF.TexType.Cubemap&&native.Format==100&&native.Header.DXGIFormat==95&&native.Header.TextureCount==6,"verified native BC6H cube required");
        var dds=source.HeaderizeExt(out var extension);var h=new DDS(dds);
        Need(extension==".dds"&&(int)h.GetDXGIFormat()==95&&h.dwWidth==h.dwHeight&&h.dwWidth is >=4 and <=512&&BitOperations.IsPow2((uint)h.dwWidth)&&h.dwMipMapCount is >=1 and <=10&&h.header10?.arraySize==1&&h.header10.miscFlag.HasFlag(DDS.RESOURCE_MISC.TEXTURECUBE),"unsupported source HDR cube");
        Need(h.dwMipMapCount<=1+BitOperations.Log2((uint)h.dwWidth),"source cube has excess mip levels");
        var images=Headerizer.GetDDSTextureBuffers(h,dds);Need(images.Count==6&&images.All(i=>i.subImages.Count==h.dwMipMapCount),"six complete source faces required");
        var result=new TPF.Texture(TPF.TPFPlatform.PS4){Name=name,Format=native.Format,Flags1=native.Flags1,Type=TPF.TexType.Cubemap,Mipmaps=checked((byte)h.dwMipMapCount),
            Header=new(){DXGIFormat=95,Width=checked((short)h.dwWidth),Height=checked((short)h.dwHeight),TextureCount=6,Unk1=native.Header.Unk1,Unk2=native.Header.Unk2,Remap=native.Header.Remap},
            Bytes=Headerizer.WritePS4Images(images,h,TPF.TexType.Cubemap)};
        var round=result.HeaderizeExt(out var ext);var rh=new DDS(round);var restored=Headerizer.GetDDSTextureBuffers(rh,round);
        Need(ext==".dds"&&restored.Count==6&&rh.dwMipMapCount==h.dwMipMapCount,"native HDR cube readback");
        for(int f=0;f<6;f++)for(int mip=0;mip<h.dwMipMapCount;mip++)Need(images[f].subImages[mip].AsSpan().SequenceEqual(restored[f].subImages[mip]),"HDR cube face/mip changed");
        return result;
    }
    internal static Result Apply(EnvironmentResult environment,Func<string,byte[]?> sourceRead,Func<string,byte[]?> nativeRead,string definitionsDirectory,MSB1 sourceMap) {
        Need(!environment.Rows.Any(r=>r.Kind=="BasinReflection"),"Basin rows already applied");
        var hydra=sourceMap.Parts.Enemies.Single(p=>p.EntityID==1200010);var golden=sourceMap.Parts.Enemies.Single(p=>p.EntityID==1200200);
        Need(hydra.ModelName=="c3530"&&hydra.CollisionName=="h0018B0"&&golden.ModelName=="c2711"&&golden.CollisionName=="h0017B0","source Basin actor identities changed");
        var basinCollisions=Enumerable.Range(9,13).Select(i=>sourceMap.Parts.Collisions.Single(p=>p.Name=="h"+i.ToString("D4")+"B0")).ToArray();
        Need(basinCollisions.All(c=>c.EnvLightMapSpotName=="GIEL h0001"),"source Basin collision capture ownership changed");
        var capture=sourceMap.Events.Environments.Single(e=>e.Name=="GIEL h0001");Need(capture.EntityID==Capture,"source GI capture ID changed");
        var collisionNames=basinCollisions.Select(c=>c.Name).ToHashSet(StringComparer.Ordinal);
        // These nine original mesh placements form the authored Basin scenery
        // family; m1800/shared trees and Garden placements remain untouched.
        var meshNames=Enumerable.Range(1,9).Select(i=>"m17"+i+"0B0").ToHashSet(StringComparer.Ordinal);
        Need(meshNames.All(name=>sourceMap.Parts.MapPieces.Any(p=>p.Name==name)),"source Basin mesh family changed");
        var parts=sourceMap.Parts.GetEntries().Where(p=>collisionNames.Contains(p.Name)||p is MSB1.Part.MapPiece&&meshNames.Contains(p.Name)||p is MSB1.Part.Enemy e&&collisionNames.Contains(e.CollisionName)||p is MSB1.Part.Object o&&collisionNames.Contains(o.CollisionName)).Where(p=>environment.Bindings.ContainsKey(p.Name)).ToArray();
        Need(parts.Any(p=>p.Name=="m1780B0")&&parts.Any(p=>p.EntityID==1200010),"Basin water/body environment binding unavailable");
        byte[] Source(string p)=>sourceRead(p)??throw new FileNotFoundException("Missing source Basin resource",p);
        var draw=BND3.Read(Source("param/DrawParam/a12_DrawParam.parambnd.dcx"));PARAM Table(string kind){var p=PARAM.Read(draw.Files.Single(f=>Leaf(f.Name)=="m12_"+kind+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitionsDirectory,kind+".xml")));return p;}
        var light=Table("LightBank");var fog=Table("FogBank");var controls=light.Rows.Single(r=>r.ID==0).Cells.Where(c=>c.Def.InternalName.StartsWith("env",StringComparison.Ordinal)).ToDictionary(c=>c.Def.InternalName,c=>c.Value);
        var split=BXF3.Read(Source("map/m12/GI_EnvM_m12.tpfbhd"),Source("map/m12/GI_EnvM_m12.tpfbdt"));const string captureName="GI_Env_m12_00_0001.tpf.dcx",cubeName="GI_EnvSpc_m12_00_0001_0";
        var captureBank=TPF.Read(split.Files.Single(f=>Leaf(f.Name)==captureName).Bytes);var sourceCube=captureBank.Textures.Single(t=>t.Name==cubeName);
        Need(captureBank.Textures.Any(t=>t.Name=="GI_EnvDif_m12_00_0001"),"source diffuse capture missing");
        var textureBytes=nativeRead(TexturePath)??throw new FileNotFoundException("Basin reflection requires the staged m40 environment bank; use ReadStaged before native fallback",TexturePath);var textures=TPF.Read(textureBytes);
        Need(textures.Platform==TPF.TPFPlatform.PS4&&textures.Encoding==1&&textures.Flag2==3&&!textures.Textures.Any(t=>t.Name==TextureName),"native environment bank metadata/owned cube collision");
        var ownedCube=Cube(TextureName,sourceCube,textures.Textures.First(t=>t.Type==TPF.TexType.Cubemap&&t.Format==100));textures.Textures.Add(ownedCube);
        var loose=environment.Files.Where(f=>f.Path.EndsWith(".gparam.dcx",StringComparison.Ordinal)).Select(f=>GPARAM.Read(f.Bytes)).ToArray();Need(loose.Length>0,"native environment profiles missing");
        var used=loose.SelectMany(g=>g.Params.SelectMany(p=>p.Fields).SelectMany(f=>f.Values).Select(v=>v.ID).Concat(g.UnkParamExtras.SelectMany(e=>e.Ids))).ToHashSet();int next=34000;int Allocate(){while(used.Contains(next))next++;used.Add(next);return next++;}
        int envRow=Allocate();var fogRows=environment.Calibration.SourceFog?parts.Select(p=>(int)p.FogID).Where(i=>i!=255).Distinct().Order().ToDictionary(i=>i,_=>Allocate()):new Dictionary<int,int>();
        var oldFogRows=fogRows.Keys.ToDictionary(i=>i,i=>parts.Where(p=>p.FogID==i).Select(p=>environment.Bindings[p.Name].FogParamID).Distinct().Single());
        var curves=fogRows.Keys.ToDictionary(i=>i,i=>{var r=fog.Rows.Single(r=>r.ID==i);float begin=Number(r,"fogBeginZ"),end=Number(r,"fogEndZ"),gain=Number(r,"colA")/100;Need(begin>=-100000&&end>begin&&end<=100000&&gain>=0,"source fog range");Need(new[]{Number(r,"colR"),Number(r,"colG"),Number(r,"colB")}.All(v=>v>=0&&v<=255)&&gain<=100,"source fog color budget");return(Begin:begin,Distance:(end-begin)*.5f,Color:new Vector4(MathF.Sqrt(Number(r,"colR")/255*gain),MathF.Sqrt(Number(r,"colG")/255*gain),MathF.Sqrt(Number(r,"colB")/255*gain),1));});
        byte[] Patch(byte[] bytes) {
            var g=GPARAM.Read(bytes);Need(g.Version==GPARAM.GparamVersion.V3,"native GPARAM V3 required");var env=Clone(g,"Env Map:Editor",0,envRow);Set(env,"Env Map: EnvMapId",envRow,(uint)TextureID);Set(env,"Env Map: ColorScale",envRow,1f);
            foreach(var (sourceId,id)in fogRows){var f=Clone(g,"FogParamEditor",oldFogRows[sourceId],id);var c=curves[sourceId];Set(f,"Fog-DistOffset",id,c.Begin);Set(f,"Fog-Dist",id,c.Distance);Set(f,"Fog-fogAlpha",id,1f);Set(f,"Fog-Color",id,c.Color);Set(f,"Fog-heightColor",id,Vector4.Zero);Set(f,"Fog-heightFogDensity",id,0f);Set(f,"Fog-fogContrast",id,0f);}
            return g.Write(DCX.Type.DCX_DFLT_10000_44_9);
        }
        var patched=new List<EnvironmentFile>();foreach(var file in environment.Files){if(file.Path.EndsWith(".gparambnd.dcx",StringComparison.Ordinal)){var binder=BND4.Read(file.Bytes);foreach(var f in binder.Files)f.Bytes=DCX.Decompress(Patch(f.Bytes.ToArray()));patched.Add(file with{Bytes=binder.Write(DCX.Type.DCX_DFLT_10000_44_9)});}else if(file.Path.EndsWith(".gparam.dcx",StringComparison.Ordinal))patched.Add(file with{Bytes=Patch(file.Bytes)});else throw new InvalidDataException("Unexpected Basin environment output "+file.Path);}
        // Commit only after all resources/rows have passed conversion.
        environment.Files.Clear();environment.Files.AddRange(patched);foreach(var p in parts){var binding=environment.Bindings[p.Name].DeepCopy();binding.EnvMapID=envRow;if(fogRows.TryGetValue(p.FogID,out int id))binding.FogParamID=id;environment.Bindings[p.Name]=binding;}
        environment.Rows.Add(new("BasinReflection",Capture,envRow,new(){["sourceCapture"]=captureName,["sourceCube"]=cubeName,["nativeTexture"]=TextureName,["nativeGain"]=1f},"Lossless source BC6H reflection cube; native unit gain. Source independent diffuse/specular tints and powers remain unconverted."));
        foreach(var (sourceId,id)in fogRows){var c=curves[sourceId];environment.Rows.Add(new("BasinFog",sourceId,id,new(){["start"]=c.Begin,["distance"]=c.Distance,["color"]=c.Color,["strength"]=1f},"Private Basin source near/far midpoint and source fog color. Exponential native curve approximates the source linear falloff; no invented height fog."));}
        string[] limits=["Only original Basin m1710..m1790 scenery and explicitly associated h0009..h0021 collision/actor/object placements use the owned reflection/fog rows. Shared Garden tree/cliff placements remain unchanged.","Exact source HDR BC6H blocks, six faces and mip chain are preserved; native cube orientation/material response still require in-game comparison. Source separate diffuse cube and independent envDif/envSpc RGB/intensity cannot be represented by the verified native scalar Env Map group and are reported without guessed gains.","Source directional/hemi lights and existing tone calibration are unchanged. Native exponential fog matches the source linear midpoint; source mist/water material effects are a separate geometry/material adapter. This fixture does not establish matched live lighting."];
        environment.Limitations.AddRange(limits);return new(new Dictionary<string,byte[]>{{TexturePath,textures.Write(DCX.Type.DCX_DFLT_10000_44_9)}},new(parts.Select(p=>p.Name).Order(StringComparer.Ordinal).ToArray(),envRow,fogRows,captureName,cubeName,6,ownedCube.Mipmaps,ownedCube.Header.Width,ownedCube.Header.Height,controls,limits));
    }
}
