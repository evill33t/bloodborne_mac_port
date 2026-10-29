// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text.Json;
using System.Text.RegularExpressions;

internal static partial class Program {
    static int BakeMap(Dictionary<string,string> values,Profile profile,string root){
        string Get(string key)=>values.TryGetValue(key,out var value)?value:throw new ArgumentException("Missing "+key);
        if(profile.Id is not("DS1" or "DS1R" or "DS2" or "DS2S"))throw new NotSupportedException("Map conversion currently supports DS1/Remastered and DS2/Scholar");
        var map=Get("--map");var target=Get("--target-map");foreach(var id in new[]{map,target})if(!Regex.IsMatch(id,@"\Am\d\d_\d\d_\d\d_\d\d\z"))throw new ArgumentException("Invalid map identifier");
        bool importItems=values.TryGetValue("--item-policy",out var itemPolicy);
        if(importItems&&(profile.Id!="DS1R"||itemPolicy!="ds1-bloodborne"))throw new NotSupportedException("Item policy currently supports ds1-bloodborne for DS1R only");
        var supported=profile.Id is "DS1" or "DS1R"?map is "m10_02_00_00" or "m10_01_00_00":map=="m10_04_00_00";
        if(!supported||target is not("m24_01_00_01" or "m24_00_00_00"))throw new NotSupportedException("Supported maps: DS1 Firelink/Undead Burg or DS2 Majula; targets m24_01_00_01 and m24_00_00_00");
        var native=Canonical(Get("--native-root"));var output=Canonical(Get("--output"));foreach(var source in new[]{root,native})if(output.Equals(source,StringComparison.OrdinalIgnoreCase)||output.StartsWith(source.TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new IOException("Output must be outside source game folders");
        if(File.Exists(output)||Directory.Exists(output))throw new IOException("Choose a new output folder");
        Vector3 Parse(string key){var p=Get(key).Split(',').Select(s=>float.Parse(s,System.Globalization.CultureInfo.InvariantCulture)).ToArray();if(p.Length!=3||p.Any(f=>!float.IsFinite(f)||Math.Abs(f)>10000))throw new ArgumentException("Invalid coordinate triple: "+key);return new(p[0],p[1],p[2]);}
        var from=Parse("--source-anchor");var to=Parse("--target-anchor");var offset=to-from;
        var requestedSpawnOffset=values.ContainsKey("--spawn-offset")?(Vector3?)Parse("--spawn-offset"):null;
        using var sourceMap=new StaticMapSource(root,profile.Id,map);
        var spawnOffset=requestedSpawnOffset??(sourceMap.PlayerStart is {} start?start-from+Vector3.UnitY:new Vector3(-4,1,-2));
        var spawn=to+spawnOffset;
        var sourceCollision=profile.Id is "DS2" or "DS2S"?LegacySourceCollision.Read(profile.Id,map,sourceMap.ReadAsset,offset):profile.Id=="DS1R"?SourceCollision.Read(profile.Id,map,sourceMap.ReadAsset,offset):null;
        var nativeRoot=Path.Combine(native,"dvdroot_ps4");
        var overlay=values.TryGetValue("--native-overlay",out var overlayPath)?Path.Combine(Canonical(overlayPath),"dvdroot_ps4"):null;
        string? NativeAsset(string path)=>overlay!=null&&File.Exists(Path.Combine(overlay,path))?Path.Combine(overlay,path):File.Exists(Path.Combine(nativeRoot,path))?Path.Combine(nativeRoot,path):null;
        byte[]? ReadNative(string path)=>NativeAsset(path) is {} found?File.ReadAllBytes(found):null;
        var smithbox=values.GetValueOrDefault("--smithbox")??Environment.GetEnvironmentVariable("SMITHBOX_DIR");
        if(smithbox==null&&File.Exists(Path.Combine(AppContext.BaseDirectory,"smithbox-path.txt")))smithbox=File.ReadAllText(Path.Combine(AppContext.BaseDirectory,"smithbox-path.txt")).Trim();
        var definitions=smithbox==null?null:Path.Combine(smithbox,"src/Smithbox.Data/Assets/PARAM",profile.Id,"Defs");
        if(profile.Id is "DS1" or "DS1R" && (definitions==null||!Directory.Exists(definitions)))throw new DirectoryNotFoundException("Set --smithbox or SMITHBOX_DIR to the source checkout containing PARAM definitions");
        var baseMap=target[..6]+"_00_00";var area=target[..3];
        var msb=MSBB.Read(Path.Combine(nativeRoot,"map/MapStudio",target+".msb.dcx"));var originalParts=msb.Parts.GetEntries().Count;
        var selectedSky=profile.Id is "DS1" or "DS1R"?msb.Parts.MapPieces.FirstOrDefault(p=>p.ModelName.StartsWith("m9",StringComparison.Ordinal))?.ModelName:null;var removedSky=selectedSky==null?0:MapReplacement.SelectSky(msb,selectedSky);
        if(target=="m24_00_00_00"){msb.Events.Sounds.Clear();msb.Events.Generators.Clear();}
        var pieceTemplate=msb.Parts.MapPieces.First();var removedScenery=MapReplacement.PruneScenery(msb);var parkedActors=MapReplacement.ParkActors(msb);var colTemplate=msb.Parts.Collisions.First();var pieceLighting=msb.Parts.MapPieces.First(p=>p.GparamConfig.LightSetID==0).GparamConfig.DeepCopy();
        var nativeFlver=File.ReadAllBytes(Path.Combine(nativeRoot,"map/m24_01_00_00/m24_01_00_00_004570.flver.dcx"));var originalFlver=FLVER2.Read(nativeFlver);var materialTemplate=originalFlver.Materials[0];var meshTemplate=originalFlver.Meshes.First(m=>m.MaterialIndex==0);
        var convertedTextures=new Dictionary<string,TPF.Texture>(StringComparer.OrdinalIgnoreCase);var reports=new List<string>();
        string Image(string path,string fallback,IReadOnlyDictionary<string,TPF.Texture> local,bool color=false){
            var key=ModelName(path);TPF.Texture? source=local.GetValueOrDefault(key);
            if(source==null){key="__"+fallback;if(!convertedTextures.ContainsKey(key)){var pixel=fallback=="normal"?new byte[]{128,128,255,255}:fallback=="black"?new byte[]{0,0,0,255}:new byte[]{255,255,255,255};var dds=SkinRetarget.RgbaTexture(pixel);convertedTextures[key]=new TPF.Texture(area+"_import_"+key,105,0,dds,TPF.TPFPlatform.PS4);}if(path.Length>0)reports.Add("Missing source texture: "+path+" -> "+fallback);return convertedTextures[key].Name;}
            if(color)key+="_srgb";
            if(!convertedTextures.ContainsKey(key)){var dds=source.HeaderizeExt(out var extension);var header=new DDS(dds);if(extension!=".dds"||header.dwWidth<1||header.dwHeight<1||header.dwWidth>8192||header.dwHeight>8192||dds.Length>128*1024*1024)throw new InvalidDataException("Unsupported source texture: "+key);if(color)dds=SkinRetarget.NativeSrgbColor(dds);if(fallback=="normal")dds=MapMaterial.Normal(dds,profile.Id);header=new DDS(dds);convertedTextures[key]=SkinRetarget.NativeTexture(area+"_import_"+key,dds);}
            return convertedTextures[key].Name;
        }
        FLVER2.Material ConvertMaterial(IFlverMaterial material,string materialName,IReadOnlyDictionary<string,TPF.Texture> local) {
            bool cutout=MapMaterial.Cutout(material.MTD,profile.Id),additive=MapMaterial.LightShaft(material.MTD,profile.Id);
            var nativeMaterial=new FLVER2.Material(materialName,additive?@"N:\SPRJ\data\Material\mtd\sfx\S_DiffuseX2[Dn]_Add.mtd":cutout?materialTemplate.MTD.Replace(".mtd","_e.mtd"):materialTemplate.MTD,materialTemplate.Index){GXIndex=materialTemplate.GXIndex};
            foreach(var slot in materialTemplate.Textures.Where(t=>!t.ParamName.StartsWith("g_DOL"))) {
                var fromSlot=material.Textures.FirstOrDefault(t=>MapMaterial.SameSlot(t.ParamName,slot.ParamName));
                string path;
                if(additive&&slot.ParamName=="g_BumpmapTexture")path=@"N:\SPRJ\data\Other\SysTex\SYSTEX_DummyNormal.tga";
                else if(slot.ParamName=="g_DisplacementTexture")path=slot.Path;
                else if(slot.ParamName is "g_SpecularTexture" or "g_ShininessTexture") {
                    string system=slot.ParamName=="g_SpecularTexture"?"SYSTEX_DummyReflectance":"SYSTEX_DummyShininess";
                    path=@"N:\SPRJ\data\Other\SysTex\"+system+".tga";
                } else {
                    string fallback=slot.ParamName.Contains("Bump")?"normal":"white";
                    path=@"N:\SPRJ\data\Model\map\"+area+@"\tex\"+Image(fromSlot?.Path??"",fallback,local,profile.Id=="DS1R"&&slot.ParamName=="g_DiffuseTexture")+".tif";
                }
                nativeMaterial.Textures.Add(new FLVER2.Texture(slot.ParamName,path,(fromSlot as FLVER2.Texture)?.TilingScale??slot.TilingScale,
                    slot.TilingTypeU,slot.TilingTypeV,slot.Unk14,slot.Unk18,slot.Unk1C));
            }
            return nativeMaterial;
        }
        var stage=Directory.CreateTempSubdirectory("bbmap-map-");
        try{
            void Write(string relative,byte[] bytes){var p=Path.Combine(stage.FullName,"dvdroot_ps4",relative);Directory.CreateDirectory(Path.GetDirectoryName(p)!);File.WriteAllBytes(p,bytes);}
            // Keep referenced original records for event stability; relocate original
            // visuals and parked actors away from the imported level.
            foreach(var part in msb.Parts.GetEntries().Where(p=>p is not MSBB.Part.Player && p is not MSBB.Part.Collision && p.ModelName?.StartsWith("m9")!=true)) {
                part.Position+=new Vector3(10000,0,0);
                Array.Clear(part.DrawGroups);Array.Clear(part.DispGroups);Array.Clear(part.BackreadGroups);
            }
            var collision=new List<MapCollision.Triangle>();var baked=new Dictionary<string,(string Name,FLVER2 Model,HashSet<int> NonSolid)>();int next=70000,totalVertices=0,totalTriangles=0;
            var modelSkips=new Dictionary<string,(string Status,string Reason)>(StringComparer.Ordinal);
            var staticTargets=new Dictionary<string,MSBB.Part.MapPiece>(StringComparer.Ordinal);
            float Setting(string key,float fallback)=>values.TryGetValue(key,out var setting)?float.Parse(setting,System.Globalization.CultureInfo.InvariantCulture):fallback;
            var calibration=new EnvironmentCalibration(SourceFog:values.GetValueOrDefault("--fog-mode","calibrated") switch {"calibrated"=>true,"native"=>false,_=>throw new ArgumentException("Expected fog-mode calibrated or native")},
                ToneEV:Setting("--exposure-ev",-.4f),FogStrength:Setting("--fog-strength",.30f),FogDistance:Setting("--fog-distance",80f),FogStart:Setting("--fog-start",25f),FogColorScale:Setting("--fog-color-scale",.35f));
            var allPlacements=sourceMap.Placements;
            MSB1? legacyMap=profile.Id is "DS1" or "DS1R"?MSB1.Read(sourceMap.ReadAsset($"map/MapStudio/{map}.msb")??sourceMap.ReadAsset($"map/MapStudio/{map}.msb.dcx")??throw new FileNotFoundException("Source MSB unavailable")):null;
            var environment=legacyMap==null?null:MapEnvironment.Load(sourceMap.ReadAsset,path=>File.Exists(Path.Combine(nativeRoot,path))?File.ReadAllBytes(Path.Combine(nativeRoot,path)):null,profile.Id,map,target,definitions!,
                legacyMap.Parts.GetEntries().Where(p=>p is MSB1.Part.MapPiece or MSB1.Part.Object or MSB1.Part.Collision or MSB1.Part.Enemy)
                    .Select(p=>new EnvironmentBinding(p.Name,p.LightID,p.FogID,p.ScatterID,p.ShadowID,p is MSB1.Part.Collision,p.ToneMapID,p.ToneCorrectID)).ToArray(),calibration,target=="m24_00_00_00"?"m24_01_00_00":null);
            if(environment!=null)foreach(var file in environment.Files)Write(file.Path,file.Bytes);
            MSBB.Part.GparamConfigStruct Lighting(string name)=>environment?.Bindings.GetValueOrDefault(name)?.DeepCopy()??pieceLighting.DeepCopy();
            var sourceParts=legacyMap?.Parts.GetEntries().ToDictionary(p=>p.Name,StringComparer.Ordinal);
            void SourceGroups(MSBB.Part targetPart,string sourceName,bool backread=true) {
                if(sourceParts==null||!sourceParts.TryGetValue(sourceName,out var sourcePart))return;
                Array.Clear(targetPart.DrawGroups);Array.Copy(sourcePart.DrawGroups,targetPart.DrawGroups,sourcePart.DrawGroups.Length);
                Array.Clear(targetPart.DispGroups);Array.Copy(sourcePart.DispGroups,targetPart.DispGroups,sourcePart.DispGroups.Length);
                if(backread){Array.Clear(targetPart.BackreadGroups);Array.Copy(sourcePart.DrawGroups,targetPart.BackreadGroups,sourcePart.DrawGroups.Length);}
            }
            BND4? materialBinder=null;List<string> skyLimitations=[];
            int sourceSkyVertices=0;

            MapBreakables.Export? breakables=null;
            if(profile.Id=="DS1R") {
                var urnTextures=new Dictionary<string,TPF.Texture>(StringComparer.OrdinalIgnoreCase);
                foreach(var model in allPlacements.Select(p=>p.Model).Where(m=>m is "o1495" or "o1496" or "o1497").Distinct())
                    foreach(var pair in sourceMap.TexturesForModel(model))urnTextures.TryAdd(pair.Key,pair.Value);
                breakables=MapBreakables.BakeFirelink(root,profile.Id,nativeRoot,msb,allPlacements.Select(p=>new MapBreakables.Placement(p.Name,p.Model,p.Position,p.Rotation,p.Scale)).ToArray(),offset,originalFlver,m=>ConvertMaterial(m,"Imported_breakable",urnTextures),ReadNative("param/gameparam/gameparam.parambnd.dcx"));
                msb.Models.Objects.AddRange(breakables.Models);msb.Parts.Objects.AddRange(breakables.Objects);
                foreach(var (path,bytes) in breakables.Files)Write(path,bytes);
                foreach(var (sourceName,actorName) in breakables.SourceActors){var actor=breakables.Objects.Single(p=>p.Name==actorName);actor.GparamConfigStruct=Lighting(sourceName);SourceGroups(actor,sourceName);}
            }
            var placements=allPlacements.Where(p=>breakables?.ConsumedNames.Contains(p.Name)!=true).ToArray();
            string ModelKey(StaticPlacement p)=>SourceCorpsePose.Supports(profile.Id,p.Model,p.InitAnimID)?p.Model+"_pose"+p.InitAnimID:p.Model;
            int posedCorpses=0;
            foreach(var placementGroup in placements.GroupBy(ModelKey)) {
                var first=placementGroup.First();var modelName=first.Model;
                if(profile.Id is "DS1" or "DS1R" && modelName.StartsWith("m9",StringComparison.Ordinal)) {
                    var skyData=sourceMap.ReadModel(modelName)??throw new FileNotFoundException("Source sky unavailable: "+modelName);
                    var sourceMaterials=BND3.Read(sourceMap.ReadAsset("mtd/Mtd.mtdbnd.dcx")??throw new FileNotFoundException("Source material definitions unavailable"));
                    materialBinder??=BND4.Read(ReadNative("mtd/allmaterialbnd.mtdbnd.dcx")??throw new FileNotFoundException("Native material definitions unavailable"));
                    MTD Definition(IReadOnlyList<BinderFile> files,string path)=>MTD.Read(files.Single(f=>Leaf(f.Name).Equals(Leaf(path),StringComparison.OrdinalIgnoreCase)).Bytes);
                    var skyTextures=sourceMap.TexturesForModel(modelName);
                    var sky=MapEnvironment.BakeSky(SourceFlver.Read(skyData,profile.Id),FLVER2.Read(ReadNative("map/m24_01_00_00/m24_01_00_00_900000.flver.dcx")??throw new FileNotFoundException("Native sky template unavailable")),
                        path=>Definition(sourceMaterials.Files,path),path=>Definition(materialBinder.Files,path),
                        path=>@"N:\SPRJ\data\Model\map\"+area+@"\tex\"+Image(path,"white",skyTextures,profile.Id=="DS1R")+".tif","BBImport_"+map);
                    foreach(var material in sky.Materials) {
                        if(materialBinder.Files.Any(f=>Leaf(f.Name).Equals(material.Name,StringComparison.OrdinalIgnoreCase)))throw new InvalidDataException("Imported sky material name already exists");
                        materialBinder.Files.Add(new BinderFile(SoulsFormats.Binder.FileFlags.Flag1,materialBinder.Files.Max(f=>f.ID)+1,@"N:\SPRJ\data\Material\mtd\map\BBImport\"+material.Name,material.Definition.Write()));
                    }
                    var skyID="m990000";if(msb.Models.MapPieces.Any(m=>m.Name==skyID))throw new InvalidDataException("Imported sky model ID is occupied");
                    baked.Add(placementGroup.Key,(skyID,sky.Model,Enumerable.Range(0,sky.Model.Meshes.Count).ToHashSet()));
                    msb.Models.MapPieces.Add(new MSBB.Model.MapPiece(skyID));
                    Write($"map/{baseMap}/{baseMap}_{skyID[1..]}.flver.dcx",sky.Model.Write(DCX.Type.DCX_DFLT_10000_44_9));
                    msb.Parts.MapPieces.RemoveAll(p=>p.ModelName==selectedSky&&p.EntityID==-1);
                    selectedSky=skyID;skyLimitations.AddRange(sky.Limitations);sourceSkyVertices+=sky.Model.Meshes.Sum(m=>m.Vertices.Count);continue;
                }
                var data=sourceMap.ReadModel(modelName);if(!data.HasValue){reports.Add("Unavailable static model: "+modelName);modelSkips.Add(placementGroup.Key,("missing-data","Source model data is unavailable; no target part was emitted."));continue;}
                var raw=Unpack(data.Value);IFlver source;
                if(SourceCorpsePose.Supports(profile.Id,modelName,first.InitAnimID)) {source=SourceCorpsePose.Load(sourceMap.ReadAsset,profile.Id,modelName,first.InitAnimID);posedCorpses+=placementGroup.Count();}
                else source=FLVER2.Is(raw.ToArray())?SourceFlver.Read(raw,profile.Id):FLVER0.Is(raw.ToArray())?FLVER0.Read(raw):throw new InvalidDataException("No FLVER geometry: "+modelName);
                var local=sourceMap.TexturesForModel(modelName);
                var result=FLVER2.Read(nativeFlver);result.Meshes.Clear();result.Materials.Clear();result.Nodes.Clear();result.Nodes.Add(new FLVER.Node{Name="root",ParentIndex=-1,Scale=Vector3.One});result.Dummies.Clear();var geometry=new StaticMapGeometry(source,profile.Id);var nonSolid=new HashSet<int>();
                foreach(var mesh in source.Meshes){var material=source.Materials[mesh.MaterialIndex];if(SkipMaterial(material.MTD,material.Name,true))continue;if(profile.Id is "DS1" or "DS1R" && material.MTD.Contains("water",StringComparison.OrdinalIgnoreCase)){reports.Add("Water surface omitted: "+material.MTD);continue;}
                    bool cutout=MapMaterial.Cutout(material.MTD,profile.Id),additive=MapMaterial.LightShaft(material.MTD,profile.Id);
                    if(additive)reports.Add("Native additive light-shaft approximation (source vertex/texture alpha retained; source angle fade and UV scrolling not ported): "+modelName);
                    var nativeMaterial=ConvertMaterial(material,"Imported_"+result.Materials.Count,local);
                    var outputMesh=new FLVER2.Mesh{UseBoneWeights=false,MaterialIndex=result.Materials.Count,NodeIndex=meshTemplate.NodeIndex};foreach(var buffer in meshTemplate.VertexBuffers)outputMesh.VertexBuffers.Add(new FLVER2.VertexBuffer(buffer.LayoutIndex){BufferIndex=buffer.BufferIndex});
                    foreach(var vertex in mesh.Vertices)outputMesh.Vertices.Add(geometry.Bake(mesh,vertex,additive));
                    foreach(var face in StaticMapGeometry.Faces(source,mesh)){var indices=face.Indices;if(indices.Count%3!=0||indices.Any(i=>i<0||i>=mesh.Vertices.Count))throw new InvalidDataException("Invalid source triangle index");foreach(var level in new[]{FLVER2.FaceSet.FSFlags.None,FLVER2.FaceSet.FSFlags.LodLevel1,FLVER2.FaceSet.FSFlags.LodLevel2})outputMesh.FaceSets.Add(new(level,false,face.Cull,face.Unknown,new List<int>(indices)));totalTriangles+=indices.Count/3;}
                    if(outputMesh.Vertices.Count==0||outputMesh.FaceSets.Count==0)continue;outputMesh.BoundingBox=new(){Min=outputMesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Min),Max=outputMesh.Vertices.Select(v=>v.Position).Aggregate(Vector3.Max)};result.Materials.Add(nativeMaterial);if(additive||MapMaterial.NonSolidFoliage(material.MTD,material.Textures.FirstOrDefault(t=>MapMaterial.SameSlot(t.ParamName,"g_DiffuseTexture"))?.Path??"",profile.Id))nonSolid.Add(result.Meshes.Count);result.Meshes.Add(outputMesh);totalVertices+=outputMesh.Vertices.Count;
                }
                if(result.Meshes.Count==0){modelSkips.Add(placementGroup.Key,source.Meshes.Count==0
                    ?("source-no-render-geometry","Source FLVER contains no meshes; no target part was emitted. Source events and interaction behavior are not ported.")
                    :("filtered-all","Source meshes produced no supported render geometry after material and triangle filtering; no target part was emitted."));continue;}
                result.Header.BoundingBoxMin=result.Meshes.Select(m=>m.BoundingBox.Min).Aggregate(Vector3.Min);result.Header.BoundingBoxMax=result.Meshes.Select(m=>m.BoundingBox.Max).Aggregate(Vector3.Max);result.Nodes[0].BoundingBoxMin=result.Header.BoundingBoxMin;result.Nodes[0].BoundingBoxMax=result.Header.BoundingBoxMax;
                while(msb.Models.MapPieces.Any(m=>m.Name=="m"+next.ToString("D6")))++next;var id="m"+(next++).ToString("D6");baked.Add(placementGroup.Key,(id,result,nonSolid));Write($"map/{baseMap}/{baseMap}_{id[1..]}.flver.dcx",result.Write(DCX.Type.DCX_DFLT_10000_44_9));msb.Models.MapPieces.Add(new MSBB.Model.MapPiece(id));
            }
            Matrix4x4 Transform(StaticPlacement p)=>Matrix4x4.CreateScale(p.Scale)*Matrix4x4.CreateRotationX(p.Rotation.X*MathF.PI/180)*Matrix4x4.CreateRotationZ(p.Rotation.Z*MathF.PI/180)*Matrix4x4.CreateRotationY(p.Rotation.Y*MathF.PI/180)*Matrix4x4.CreateTranslation(p.Position+offset);
            int placed=0;foreach(var part in placements){if(!baked.TryGetValue(ModelKey(part),out var model))continue;var p=(MSBB.Part.MapPiece)pieceTemplate.DeepCopy();p.Name=model.Name+"_"+placed.ToString("D4");p.ModelName=model.Name;p.Position=part.Position+offset;p.Rotation=part.Rotation;p.Scale=part.Scale;p.EntityID=-1;p.InstanceID=placed++;Array.Fill(p.DrawGroups,uint.MaxValue);Array.Fill(p.BackreadGroups,uint.MaxValue);Array.Fill(p.DispGroups,0u);p.ShadowSource=part.ShadowSource??false;p.ShadowDest=part.ShadowDest??p.ShadowDest;p.IsShadowOnly=part.ShadowOnly??p.IsShadowOnly;p.GparamConfig=Lighting(part.Name);msb.Parts.MapPieces.Add(p);staticTargets.Add(part.Name,p);SourceGroups(p,part.Name);
                // Source H/L collision is preferred. Only unsupported source formats
                // fall back to filtered render triangles.
                if(sourceCollision!=null)continue;
                var transform=Transform(part);foreach(var mesh in model.Model.Meshes.Where((m,i)=>!model.NonSolid.Contains(i)))foreach(var face in mesh.FaceSets.Where(f=>f.Flags==FLVER2.FaceSet.FSFlags.None)){var ix=face.Indices;for(int i=0;i+2<ix.Count;i+=3){var a=Vector3.Transform(mesh.Vertices[ix[i]].Position,transform);var b=Vector3.Transform(mesh.Vertices[ix[i+1]].Position,transform);var c=Vector3.Transform(mesh.Vertices[ix[i+2]].Position,transform);if(Vector3.Cross(b-a,c-a).LengthSquared()>1e-10f)collision.Add(new(a,c,b));}}
            }
            if(materialBinder!=null)Write("mtd/allmaterialbnd.mtdbnd.dcx",materialBinder.Write(DCX.Type.DCX_DFLT_10000_44_9));
            // A larger source map may need more collision records than its native donor.
            int required=sourceCollision?.Parts.Count(p=>p.Layer=="h"&&p.PlayerSolid)??0;
            var collisionModels=msb.Models.Collisions.Select(m=>m.Name).ToHashSet(StringComparer.Ordinal);
            for(int id=10000;msb.Parts.Collisions.DistinctBy(p=>p.ModelName).Count()<required;id++) {
                if(id>65535)throw new InvalidDataException("Native collision model namespace exhausted");
                string name="h"+id.ToString("D6");if(!collisionModels.Add(name))continue;
                var col=(MSBB.Part.Collision)colTemplate.DeepCopy();col.Name=name;col.ModelName=name;col.EntityID=-1;
                var model=(MSBB.Model.Collision)msb.Models.Collisions.First().DeepCopy();model.Name=name;msb.Models.Collisions.Add(model);msb.Parts.Collisions.Add(col);
            }
            var slots=msb.Parts.Collisions.DistinctBy(p=>p.ModelName).ToArray();
            var boundCollision=sourceCollision==null?null:MapCollisionBindings.Partition(sourceCollision,slots.Length);
            foreach(var col in msb.Parts.Collisions){col.Position=Vector3.Zero;col.Rotation=Vector3.Zero;col.Scale=Vector3.One;col.DisableStart=false;col.HitFilterID=8;Array.Fill(col.DrawGroups,uint.MaxValue);Array.Fill(col.DispGroups,uint.MaxValue);col.GparamConfigStruct=colTemplate.GparamConfigStruct.DeepCopy();col.SceneGparamConfigStruct=colTemplate.SceneGparamConfigStruct.DeepCopy();}
            var clipped=collision.Where(t=>{var lo=Vector3.Min(t.A,Vector3.Min(t.B,t.C));var hi=Vector3.Max(t.A,Vector3.Max(t.B,t.C));return hi.Y<=to.Y+200&&lo.Y>=to.Y-210&&hi.X>=to.X-255&&lo.X<=to.X+255&&hi.Z>=to.Z-240&&lo.Z<=to.Z+260;}).ToList();
            var highTriangles=sourceCollision?.PlayerTriangles("h")??clipped;
            var lowTriangles=sourceCollision?.PlayerTriangles("l")??clipped;
            if(highTriangles.Count==0||lowTriangles.Count==0)throw new InvalidDataException("Imported map must have both high and low solid collision");
            if(boundCollision!=null)for(int i=0;i<slots.Length;i++)foreach(var col in msb.Parts.Collisions.Where(c=>c.ModelName==slots[i].ModelName)) {
                col.GparamConfigStruct=Lighting(boundCollision[i].SourceName);SourceGroups(col,boundCollision[i].SourceName,false);
                if(environment?.ShadowBindings.TryGetValue(boundCollision[i].SourceName,out int shadowID)==true)col.SceneGparamConfigStruct.ShadowParamID=shadowID;
                if(environment?.SceneBindings.TryGetValue(boundCollision[i].SourceName,out var scene)==true){col.SceneGparamConfigStruct.ToneMapID=scene.ToneMapID;if(scene.ColorGradingID is {} grade)col.SceneGparamConfigStruct.ColorGradingID=grade;}
            }
            if(breakables!=null)foreach(var actor in breakables.Objects)actor.CollisionName=slots[0].Name;
            MapCharacters.Export? characters=null;
            if(profile.Id is "DS1" or "DS1R" && map=="m10_02_00_00") {
                characters=MapCharacters.BakeFirelink(sourceMap.ReadAsset,profile.Id,map,definitions!,nativeRoot,msb,offset,slots[0].Name,breakables?.Files.GetValueOrDefault("param/gameparam/gameparam.parambnd.dcx")??ReadNative("param/gameparam/gameparam.parambnd.dcx"));
                msb.Models.Enemies.AddRange(characters.Models);msb.Parts.Enemies.AddRange(characters.Actors);
                foreach(var actorReport in characters.Reports.Where(r=>r.EntityID.HasValue)){var actor=characters.Actors.Single(p=>p.EntityID==actorReport.EntityID);actor.GparamConfigStruct=Lighting(actorReport.SourceName);SourceGroups(actor,actorReport.SourceName);}
                foreach(var (path,bytes) in characters.Files)Write(path,bytes);
            }
            MapEnemySpawns.Result? enemySpawns=null;
            if(profile.Id=="DS1R"&&legacyMap!=null&&boundCollision!=null) {
                enemySpawns=MapEnemySpawns.Bake(sourceMap.ReadAsset,legacyMap,map,definitions!,nativeRoot,msb,offset,boundCollision,slots,
                    characters?.GameParamBytes??breakables?.Files.GetValueOrDefault("param/gameparam/gameparam.parambnd.dcx")??ReadNative("param/gameparam/gameparam.parambnd.dcx")!,nativeMap:baseMap,readNative:ReadNative);
                foreach(var spawnReport in enemySpawns.Spawns){var actor=msb.Parts.Enemies.Single(p=>p.EntityID==spawnReport.EntityID);actor.GparamConfigStruct=Lighting(spawnReport.SourceName);SourceGroups(actor,spawnReport.SourceName);}
                foreach(var (path,bytes) in enemySpawns.Files)Write(path,bytes);
            }
            MapItems.Result? items=null;
            if(importItems&&legacyMap!=null&&boundCollision!=null&&enemySpawns!=null) {
                items=MapItems.Bake(sourceMap.ReadAsset,ReadNative,legacyMap,msb,map,definitions!,nativeRoot,offset,boundCollision,slots,
                    enemySpawns.Spawns,enemySpawns.Files.GetValueOrDefault("param/gameparam/gameparam.parambnd.dcx")??
                    characters?.GameParamBytes??breakables?.Files.GetValueOrDefault("param/gameparam/gameparam.parambnd.dcx")??ReadNative("param/gameparam/gameparam.parambnd.dcx")!);
                foreach(var pickup in items.Pickups)msb.Parts.Objects.Single(p=>p.Name==pickup.TargetPart).GparamConfigStruct=Lighting(pickup.SourcePart);
                foreach(var (path,bytes) in items.Files)Write(path,bytes);
            }
            foreach(var prefix in new[]{"h","l"}){
                var groups=boundCollision==null?MapCollision.Partition(prefix=="h"?highTriangles:lowTriangles,slots.Length):boundCollision.Select(c=>(prefix=="h"?c.High:c.Low).ToList()).ToList();
                var path=Path.Combine(nativeRoot,"map",baseMap,prefix+baseMap[1..]+".hkx");var archive=BXF4.Read(path+"bhd",path+"bdt");
                // Reuse the separately validated player-solid H/L donor. The
                // first body of another map can carry different physical flags.
                var reference=Path.Combine(nativeRoot,"map/m24_01_00_00",prefix+"24_01_00_00.hkx");
                using var templates=new BXF4Reader(File.ReadAllBytes(reference+"bhd"),reference+"bdt");
                var template=templates.ReadFile(templates.Files.Single(f=>f.Name.EndsWith("000010.hkx.dcx",StringComparison.Ordinal))).ToArray();const string templateName="000010";
                var converted=new Dictionary<string,byte[]>();for(int i=0;i<slots.Length;++i){var number=slots[i].ModelName[1..];var hkx=MapCollision.Bake(template,groups[i]);hkx=MapCollision.RenameBody(hkx,prefix+templateName,prefix+number);converted[number]=DCX.Compress(hkx,DCX.Type.DCX_DFLT_10000_44_9);}
                void Replace(BXF4 binder,bool add){
                    var done=new HashSet<string>();foreach(var file in binder.Files){var number=Leaf(file.Name).Split('_').Last().Split('.')[0];if(converted.TryGetValue(number,out var bytes)){file.Bytes=bytes;done.Add(number);}else file.Bytes=DCX.Compress(MapCollision.Translate(file.Bytes.ToArray(),new Vector3(0,-10000,0)),DCX.Type.DCX_DFLT_10000_44_9);}
                    if(add)foreach(var (number,bytes) in converted)if(!done.Contains(number))binder.Files.Add(new BinderFile(SoulsFormats.Binder.FileFlags.Flag1,binder.Files.Max(f=>f.ID)+1,baseMap+"\\"+prefix+baseMap[1..]+"_"+number+".hkx.dcx",bytes));
                }
                Replace(archive,true);archive.Write(out var header,out var body);Write($"map/{baseMap}/{prefix}{baseMap[1..]}.hkxbhd",header);Write($"map/{baseMap}/{prefix}{baseMap[1..]}.hkxbdt",body);
                if(File.Exists(path+"bhd.patch")){var patch=BXF4.Read(path+"bhd.patch",path+"bdt.patch");Replace(patch,false);patch.Write(out var ph,out var pd);Write($"map/{baseMap}/{prefix}{baseMap[1..]}.hkxbhd.patch",ph);Write($"map/{baseMap}/{prefix}{baseMap[1..]}.hkxbdt.patch",pd);}
            }
            var lamp=msb.Parts.Objects.SingleOrDefault(p=>p.EntityID==2411950);if(lamp!=null){lamp.Position=to+new Vector3(2,-.1f,0);lamp.Rotation=Vector3.Zero;lamp.CollisionName=slots[0].Name;Array.Fill(lamp.DrawGroups,uint.MaxValue);Array.Fill(lamp.BackreadGroups,uint.MaxValue);}
            foreach(var player in msb.Parts.GetEntries().OfType<MSBB.Part.Player>()){player.Position=spawn;player.Rotation=Vector3.Zero;}
            foreach(var point in msb.Events.GetEntries().OfType<MSBB.Event.SpawnPoint>()){var region=msb.Regions.GetEntries().Single(r=>r.Name==point.SpawnPointName);region.Position=spawn;region.Rotation=Vector3.Zero;}
            var textures=BXF4.Read(ReadNative($"map/{area}/{area}_0003.tpfbhd")!,ReadNative($"map/{area}/{area}_0003.tpfbdt")!);int nextTexture=textures.Files.Max(f=>f.ID)+1;foreach(var texture in convertedTextures.Values){var tpf=new TPF{Platform=TPF.TPFPlatform.PS4,Encoding=1,Flag2=3};tpf.Textures.Add(texture);var encoded=tpf.Write(DCX.Type.DCX_DFLT_10000_44_9);var existing=textures.Files.SingleOrDefault(f=>Leaf(f.Name)==texture.Name+".tpf.dcx");if(existing==null)textures.Files.Add(new BinderFile(SoulsFormats.Binder.FileFlags.Flag1,nextTexture++,texture.Name+".tpf.dcx",encoded));else if(!existing.Bytes.Span.SequenceEqual(encoded))throw new InvalidDataException("Shared imported texture conflicts: "+texture.Name);}textures.Write(out var texHeader,out var texData);Write($"map/{area}/{area}_0003.tpfbhd",texHeader);Write($"map/{area}/{area}_0003.tpfbdt",texData);
            byte[]? ReadComposed(string path)=>items?.Files.GetValueOrDefault(path)??ReadNative(path);
            var areaName=MapAreaName.Export(msb,profile.Id,map,nativeRoot,sourceMap.ReadAsset,Write,ReadComposed);
            var destinations=values.TryGetValue("--connection-targets",out var connectionsPath)?JsonSerializer.Deserialize<Dictionary<string,string>>(File.ReadAllText(connectionsPath))!:new Dictionary<string,string>();
            MapRuntimeLinks.Result? runtimeLinks=null;
            if(legacyMap!=null&&boundCollision!=null)runtimeLinks=MapRuntimeLinks.Apply(msb,legacyMap,boundCollision,slots,destinations);
            else msb.Parts.ConnectCollisions.Clear();
            Write("map/MapStudio/"+target+".msb.dcx",msb.Write(DCX.Type.DCX_DFLT_10000_44_9));
            if(target=="m24_00_00_00") {
                // Both Cathedral Ward story variants are reserved by this import.
                // They must resolve to the same imported scene at either phase.
                Write("map/MapStudio/m24_00_00_01.msb.dcx",msb.Write(DCX.Type.DCX_DFLT_10000_44_9));
                var script=EMEVD.Read(ReadNative("event/m24_00_00_00.emevd.dcx")??throw new FileNotFoundException("Native map script unavailable"));
                script.Events.Clear();Write("event/m24_00_00_00.emevd.dcx",script.Write(DCX.Type.DCX_DFLT_10000_44_9));
            } else foreach(var variantPath in Directory.EnumerateFiles(Path.Combine(nativeRoot,"map/MapStudio"),target[..6]+"_*.msb.dcx")) {
                if(Path.GetFileName(variantPath)==target+".msb.dcx")continue;
                // Shared Havok bodies keep each variant's own names/references,
                // while source collision metadata and links follow body identity.
                var variant=MSBB.Read(File.ReadAllBytes(variantPath));
                if(legacyMap!=null&&boundCollision!=null)MapRuntimeLinks.Apply(variant,legacyMap,boundCollision,MapRuntimeLinks.MirrorCollisionBindings(variant,slots),destinations);
                else variant.Parts.ConnectCollisions.Clear();
                Write("map/MapStudio/"+Path.GetFileName(variantPath),variant.Write(DCX.Type.DCX_DFLT_10000_44_9));
            }
            var breakableTargets=breakables?.Objects.ToDictionary(p=>p.Name,StringComparer.Ordinal);
            var placementResults=allPlacements.Select(part=>{
                MSBB.Part? targetPart=null;string status;string? reason=null;
                if(staticTargets.TryGetValue(part.Name,out var staticPart)){targetPart=staticPart;status="placed-static";}
                else if(breakables?.SourceActors.TryGetValue(part.Name,out var actorName)==true){targetPart=breakableTargets![actorName];status="placed-breakable";reason="Source visuals use native approximate fracture physics; source object events and interactions are not ported.";}
                else if(modelSkips.TryGetValue(ModelKey(part),out var skipped)){status=skipped.Status;reason=skipped.Reason;}
                else throw new InvalidDataException("Unaccounted source placement: "+part.Name);
                return new{sourceName=part.Name,sourceModel=part.Model,sourceTransform=new{position=part.Position,rotation=part.Rotation,scale=part.Scale},initAnimID=part.InitAnimID,
                    status,reason,targetName=targetPart?.Name,targetModel=targetPart?.ModelName,targetKind=targetPart?.GetType().Name,
                    targetTransform=targetPart==null?null:new{position=targetPart.Position,rotation=targetPart.Rotation,scale=targetPart.Scale}};
            }).ToArray();
            int PlacementCount(string status)=>placementResults.Count(p=>p.status==status);
            int placedStatic=PlacementCount("placed-static"),placedBreakable=PlacementCount("placed-breakable"),sourceNoRenderGeometry=PlacementCount("source-no-render-geometry"),
                filteredAll=PlacementCount("filtered-all"),missingData=PlacementCount("missing-data");
            int accounted=placedStatic+placedBreakable+sourceNoRenderGeometry+filteredAll+missingData;
            var placementSummary=new{total=allPlacements.Count,placedStatic,placedBreakable,sourceNoRenderGeometry,filteredAll,missingData,accounted,
                reconciled=accounted==allPlacements.Count&&placedStatic==placed&&placedBreakable==(breakables?.Objects.Count??0)};
            var report=new{source=profile.Id,map,target,from,to,offset,models=baked.Count,placements=placed,vertices=totalVertices,modelTriangles=totalTriangles,collisionTriangles=highTriangles.Count,lowCollisionTriangles=lowTriangles.Count,collisionChunks=slots.Length,
                placementSummary,placementResults,runtimeLinks,items=items==null?null:new{items.Lots,items.Pickups,items.Skipped,items.QuestItems,items.VialDropLot},
                collisionSource=sourceCollision==null?"render-mesh fallback":"original source high/low collision",
                sourceCollisionParts=sourceCollision?.Parts.Select(p=>new{p.Name,p.Model,p.Layer,p.HitFilter,p.Profile,p.PlayerSolid,p.TriangleCount,materials=p.Surfaces.Select(s=>s.Material).Distinct().ToArray()}).ToArray(),
                breakableObjects=breakables?.Objects.Count??0,breakableModels=breakables?.Reports,posedCorpses,characters=characters?.Reports,enemySpawns=enemySpawns==null?null:new{enemySpawns.Spawns,enemySpawns.Skipped,enemySpawns.Limitations},sourceSkyVertices,sourceSkyLimitations=skyLimitations,
                environmentRows=environment?.Rows,environmentCalibration=environment?.Calibration,environmentLimitations=environment?.Limitations,collisionEnvironment=boundCollision?.Select((c,i)=>new{target=slots[i].Name,source=c.SourceName}).ToArray(),areaName,areaNameBanner="not yet verified in gameplay",spawn,textures=convertedTextures.Count,originalParts,removedScenery,parkedActors,selectedSky,removedSky,nonSolidMeshes=baked.Values.Sum(v=>v.NonSolid.Count),finalMapPieces=msb.Parts.MapPieces.Count,notes=reports.Distinct().ToArray(),limitations="Experimental map replacement: source high/low solid collision where supported; converted source light sets/scattering and sky where supported, using native Bloodborne material shaders. Source AI, quests, water physics, collision material behavior and navigation are not ported. Imported urns use native approximate fracture physics; verified corpse clips are baked to static poses. Terrain blending uses its first texture layer. Unused original scenery removed; native actors parked as dummy records; referenced scenery and props relocated horizontally outside the play area; unused native collision bodies moved below the scene. Source sky/cloud geometry and texture colors imported for DS1; see environment and sky limitations. Spawn regions point to the imported map; lantern travel remains unverified. Use an isolated copied save."};
            File.WriteAllText(Path.Combine(stage.FullName,"conversion.json"),JsonSerializer.Serialize(report,new JsonSerializerOptions{WriteIndented=true,IncludeFields=true}));File.WriteAllText(Path.Combine(stage.FullName,"README.txt"),$"Experimental {map} map replacement for Bloodborne {target}. Enable only with a copied test save. Place player near {to.X} {to.Y} {to.Z}. See conversion.json for coverage and limitations.\n");Directory.CreateDirectory(Path.GetDirectoryName(output)!);Directory.Move(stage.FullName,output);Console.WriteLine(JsonSerializer.Serialize(report,new JsonSerializerOptions{IncludeFields=true}));return 0;
        }catch{if(stage.Exists)stage.Delete(true);throw;}
    }
}
