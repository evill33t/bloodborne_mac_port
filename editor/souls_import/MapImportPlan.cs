// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;

internal static partial class Program {
    // An inventory is not an assertion that every source subsystem is converted.
    // The same plan accompanies each repeatable import so omitted dependencies stay visible.
    static int PlanMap(Dictionary<string,string> values,Profile profile,string root) {
        var map=values.GetValueOrDefault("--map","");
        if(!Regex.IsMatch(map,@"\Am\d\d_\d\d_\d\d_\d\d\z"))throw new ArgumentException("Invalid map identifier");
        using var assets=new Assets(root,profile.Id);
        var path=profile.Id is "DS2" or "DS2S"?$"map/{map}/{map}.msb":$"map/MapStudio/{map}.msb.dcx";
        var bytes=assets.Read(path)??assets.Read(path.Replace(".dcx",""))??throw new FileNotFoundException("Source map unavailable",path);
        var reader=Reader(profile)??throw new NotSupportedException("Source map reader unavailable");
        var method=reader.GetMethods(BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy).Single(m=>m.Name=="Read"&&m.GetParameters().Length==1&&m.GetParameters()[0].ParameterType==typeof(Memory<byte>));
        var msb=(IMsb)method.Invoke(null,[new Memory<byte>(bytes)])!;
        object? Field(object item,string name)=>item.GetType().GetProperty(name)?.GetValue(item);
        Dictionary<string,object?> Fields(object item,params string[] names)=>names.Where(n=>item.GetType().GetProperty(n)!=null).ToDictionary(n=>n,n=>Field(item,n) is byte[] components?components.Select(c=>(int)c).ToArray():Field(item,n));
        MapConnections.Inventory? topology=null;
        if(profile.Id is "DS1" or "DS1R" or "DS2" or "DS2S") {
            var maps=new HashSet<string>(StringComparer.Ordinal);
            foreach(var prefix in new[]{root,Path.Combine(root,"dvdroot_ps4")}) {
                var folder=Path.Combine(prefix,"map", "MapStudio");
                if(Directory.Exists(folder))foreach(var file in Directory.EnumerateFiles(folder,"*.msb*"))maps.Add(Path.GetFileName(file).Split('.')[0]);
                if(profile.Id is "DS2" or "DS2S" && Directory.Exists(Path.Combine(prefix,"map")))foreach(var dir in Directory.EnumerateDirectories(Path.Combine(prefix,"map")))maps.Add(Path.GetFileName(dir));
            }
            var catalog=maps.Where(m=>Regex.IsMatch(m,@"\Am\d\d_\d\d_\d\d_\d\d\z")).Select(m=>new MapConnections.SourceIdentity(profile.Id,m)).ToArray();
            topology=MapConnections.Read(bytes,new(profile.Id,map),catalog.Length==0?null:catalog);
        }
        var parts=msb.Parts.GetEntries();
        var connections=parts.Where(p=>p.GetType().Name.Contains("ConnectCollision")).Select(p=>new {p.Name,p.Position,p.Rotation,values=Fields(p,"MapID","CollisionName","ModelName")}).ToArray();
        var actors=parts.Where(p=>Kind(p.GetType().Name)==2).Select(p=>new {p.Name,p.ModelName,p.Position,p.Rotation,p.Scale,kind=p.GetType().Name,values=Fields(p,"EntityID","NPCParamID","ThinkParamID","CharaInitID","InitAnimID","TalkID","CollisionName")}).ToArray();
        var objects=parts.Where(p=>Kind(p.GetType().Name)==1).Select(p=>new {p.Name,p.ModelName,p.Position,p.Rotation,p.Scale,values=Fields(p,"EntityID","InitAnimID","CollisionName")}).ToArray();
        var events=msb.Events.GetEntries();
        var sounds=events.Where(e=>e.GetType().Name.Contains("Sound")).Select(e=>new {e.Name,values=Fields(e,"SoundID","SoundType","RegionName","PartName","EntityID")}).ToArray();
        var collisions=parts.Where(p=>p.GetType().Name=="Collision").Select(p=>new {p.Name,p.ModelName,p.Position,p.Rotation,p.Scale,values=Fields(p,"HitFilterID","MapNameID","PlayRegionID","DisableStart")}).ToArray();
        var lighting=parts.Where(p=>p.GetType().Name=="MapPiece").Select(p=>Fields(p,"LightID","FogID","ScatterID","LensFlareID","ShadowID","DofID","ToneMapID","ToneCorrectID","PointLightID","LanternID")).DistinctBy(d=>JsonSerializer.Serialize(d)).ToArray();
        var data=new {schema="bbmap-import-plan-1",profile=profile.Id,map,sourceRoot=root,sourceMapPath=path,sourceMapSha256=Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(),
            partCounts=parts.GroupBy(p=>p.GetType().Name).ToDictionary(g=>g.Key,g=>g.Count()),
            models=parts.Where(p=>!string.IsNullOrEmpty(p.ModelName)).GroupBy(p=>p.ModelName).Select(g=>new {model=g.Key,placements=g.Count(),kinds=g.Select(p=>p.GetType().Name).Distinct().ToArray()}).ToArray(),
            actors,objects,collisions,connections,topology,sounds,lighting,eventCounts=events.GroupBy(e=>e.GetType().Name).ToDictionary(g=>g.Key,g=>g.Count()),
            requiredStages=new[]{"geometry/textures","high/low collision and filters","destructible objects","NPC/enemy appearance and behavior","corpse poses","localized area name","connected map loading","music and ambience","sky and lighting","items/events/quests"},
            notice="Source inventory only. Read conversion.json for implemented stages and gameplay limits. Source AI, quests, audio and map transitions require explicit conversion; their presence here does not imply runtime support."};
        var json=JsonSerializer.Serialize(data,new JsonSerializerOptions{WriteIndented=true,IncludeFields=true});
        if(values.TryGetValue("--output",out var output)) {
            output=Canonical(output);
            foreach(var game in new[]{root,values.TryGetValue("--native-root",out var n)?Canonical(n):root})
                if(output.Equals(game,StringComparison.OrdinalIgnoreCase)||output.StartsWith(game.TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new IOException("Plan output must be outside game folders");
            using var file=new FileStream(output,FileMode.CreateNew,FileAccess.Write);using var writer=new StreamWriter(file);writer.Write(json);
        } else Console.WriteLine(json);
        return 0;
    }
}
