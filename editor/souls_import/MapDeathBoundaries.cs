// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

// Authored fall sensors are separate from player-solid floors. DS1R and BB
// share MSB hit filters13/14 (fall camera) and15 (instant death). Keep the two
// source collision layers; neither a world-height cutoff nor a render mesh is
// an adequate substitute for their irregular boundaries.
internal static class MapDeathBoundaries {
    internal record Body(string Model,MSBB.Part.Collision Part,MSBB.Model.Collision Registration,byte[] High,byte[] Low);
    internal record Binding(string SourceName,int SourceEntityID,string TargetName,string TargetModel,byte HitFilter,
        bool InitialEnabled,int HighTriangles,int LowTriangles,Vector3 HighMin,Vector3 HighMax,Vector3 LowMin,Vector3 LowMax);
    internal record Report(IReadOnlyList<Binding> Boundaries,IReadOnlyList<string> Limits);
    internal record Result(IReadOnlyList<Body> Bodies,Report Report);
    sealed record Template(string Map,MSBB.Part.Collision Part,MSBB.Model.Collision Model,byte[] High,byte[] Low);

    static bool IsDeath(byte filter)=>filter is 13 or 14 or 15;
    static void Require(bool ok,string message){if(!ok)throw new InvalidDataException("Source death boundaries: "+message);}
    static (Vector3 Min,Vector3 Max) Bounds(IReadOnlyList<MapCollision.Triangle> triangles) {
        var points=triangles.SelectMany(t=>new[]{t.A,t.B,t.C});return(points.Aggregate(Vector3.Min),points.Aggregate(Vector3.Max));
    }

    static Template ReadTemplate(byte filter,Func<string,byte[]?> readNative) {
        string map=filter==14?"m24_02_00_00":"m24_01_00_00",model=filter==13?"h005000":"h005010";
        byte[] Read(string path)=>readNative(path)??throw new FileNotFoundException("Native death-boundary template unavailable: "+path);
        var msb=MSBB.Read(Read($"map/MapStudio/{map}.msb.dcx"));
        var parts=msb.Parts.Collisions.Where(p=>p.ModelName==model&&p.HitFilterID==filter).ToArray();
        var models=msb.Models.Collisions.Where(m=>m.Name==model).ToArray();
        Require(parts.Length==1&&models.Length==1,"native same-filter template identity is ambiguous");
        var part=parts[0];
        Require(!part.DisableStart&&part.EntityID==-1&&part.MapNameID==-1&&part.PlayRegionID==-1&&
            part.Position==Vector3.Zero&&part.Rotation==Vector3.Zero&&part.Scale==Vector3.One&&
            part.DrawGroups.All(v=>v==0)&&part.DispGroups.All(v=>v==0)&&part.BackreadGroups.All(v=>v==0),
            "native fall-sensor metadata differs from the verified retail contract");
        byte[] Layer(string prefix) {
            string stem=$"map/{map}/{prefix}{map[1..]}.hkx";
            using var archive=new BXF4Reader(Read(stem+"bhd"),Read(stem+"bdt"));
            var matches=archive.Files.Where(f=>Path.GetFileName(f.Name.Replace('\\','/')).EndsWith("_"+model[1..]+".hkx.dcx",StringComparison.Ordinal)).ToArray();
            Require(matches.Length==1,"native fall-sensor collision body is missing or repeated");
            return archive.ReadFile(matches[0]).ToArray();
        }
        return new(map,part,models[0],Layer("h"),Layer("l"));
    }

    // Apply after solid-slot allocation/reset, before the caller writes the H/L
    // archives. Merge each returned body's Model[1..] and High/Low into those
    // archives. No source event/flag/entity namespace is copied into the mod.
    internal static Result Apply(string profile,MSB1 source,SourceCollision.Result collision,MSBB destination,Func<string,byte[]?> readNative,string? targetMap=null) {
        if(profile!="DS1R")throw new NotSupportedException("Authored death-boundary conversion currently supports DS1 Remastered only");
        Require(targetMap==null||targetMap.Length==12&&targetMap[0]=='m'&&Enumerable.Range(1,11).All(i=>i is 3 or 6 or 9?targetMap[i]=='_':char.IsAsciiDigit(targetMap[i])),
            "invalid destination map identifier");
        var requested=source.Parts.Collisions.Where(p=>IsDeath(p.HitFilterID)).OrderBy(p=>p.Name,StringComparer.Ordinal).ToArray();
        Require(requested.Select(p=>p.Name).Distinct(StringComparer.Ordinal).Count()==requested.Length,"source fall-sensor names repeat");
        var groups=collision.Parts.Where(p=>IsDeath(p.HitFilter)).GroupBy(p=>p.Name,StringComparer.Ordinal).ToDictionary(g=>g.Key,g=>g.ToArray(),StringComparer.Ordinal);
        Require(groups.Keys.Order(StringComparer.Ordinal).SequenceEqual(requested.Select(p=>p.Name)),"decoded death boundaries differ from the source MSB");
        var modelNames=destination.Models.GetEntries().Select(m=>m.Name).ToHashSet(StringComparer.Ordinal);
        var partNames=destination.Parts.GetEntries().Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        var templates=new Dictionary<byte,Template>();var bodies=new List<Body>();var bindings=new List<Binding>();int next=60000;
        foreach(var sourcePart in requested) {
            Require(sourcePart.ModelName is {Length:>1}&&sourcePart.ModelName[0]=='h'&&sourcePart.DrawGroups.Length<=8&&sourcePart.DispGroups.Length<=8,
                "invalid source fall-sensor model/groups: "+sourcePart.Name);
            var pair=groups[sourcePart.Name];
            Require(pair.Length==2&&pair.Count(p=>p.Layer=="h")==1&&pair.Count(p=>p.Layer=="l")==1&&
                pair.All(p=>p.Profile==profile&&p.HitFilter==sourcePart.HitFilterID&&p.Model==p.Layer+sourcePart.ModelName[1..]),
                "source H/L fall-sensor pairing differs: "+sourcePart.Name);
            var high=pair.Single(p=>p.Layer=="h").Surfaces.SelectMany(s=>s.Triangles).Select(t=>t with{NativeMaterial=null}).ToArray();
            var low=pair.Single(p=>p.Layer=="l").Surfaces.SelectMany(s=>s.Triangles).Select(t=>t with{NativeMaterial=null}).ToArray();
            Require(high.Length>0&&low.Length>0,"empty authored fall sensor: "+sourcePart.Name);
            string name;do {Require(next<=65535,"native collision model namespace exhausted");name="h"+(next++).ToString("D6");}while(modelNames.Contains(name)||partNames.Contains(name));
            modelNames.Add(name);partNames.Add(name);
            if(!templates.TryGetValue(sourcePart.HitFilterID,out var template)){template=ReadTemplate(sourcePart.HitFilterID,readNative);templates.Add(sourcePart.HitFilterID,template);}
            byte[] Bake(byte[] bytes,string prefix,IReadOnlyList<MapCollision.Triangle> triangles)=>DCX.Compress(
                MapCollision.RenameBody(MapCollision.Bake(bytes,triangles),prefix+template.Model.Name[1..],prefix+name[1..]),DCX.Type.DCX_DFLT_10000_44_9);
            var highBytes=Bake(template.High,"h",high);var lowBytes=Bake(template.Low,"l",low);
            var part=(MSBB.Part.Collision)template.Part.DeepCopy();part.Name=name;part.ModelName=name;part.EntityID=-1;
            // Read() has already applied the complete authored instance transform.
            part.Position=Vector3.Zero;part.Rotation=Vector3.Zero;part.Scale=Vector3.One;part.DisableStart=sourcePart.DisableStart;
            Array.Clear(part.DrawGroups);Array.Copy(sourcePart.DrawGroups,part.DrawGroups,sourcePart.DrawGroups.Length);
            Array.Clear(part.DispGroups);Array.Copy(sourcePart.DispGroups,part.DispGroups,sourcePart.DispGroups.Length);
            var registration=(MSBB.Model.Collision)template.Model.DeepCopy();registration.Name=name;
            // Native SIB metadata names the authoring HKT, while runtime opens
            // the target HKX archive by its map/model identity. Keep this owned
            // registration coherent too; do not retain a donor map or old body.
            string metadataMap=targetMap??template.Map;
            registration.SibPath=@"N:\SPRJ\data\Model\map\"+metadataMap[..6]+@"_00_00\hkt\"+name+"A"+metadataMap.Substring(1,2)+".hkt";
            var hb=Bounds(high);var lb=Bounds(low);
            bodies.Add(new(name,part,registration,highBytes,lowBytes));
            bindings.Add(new(sourcePart.Name,sourcePart.EntityID,name,name,sourcePart.HitFilterID,!sourcePart.DisableStart,
                high.Length,low.Length,hb.Min,hb.Max,lb.Min,lb.Max));
        }
        // Validate and bake every body first: failures leave the caller's scene
        // unchanged and cannot publish only part of a death boundary.
        foreach(var body in bodies){destination.Models.Collisions.Add(body.Registration);destination.Parts.Collisions.Add(body.Part);}
        return new(bodies,new(bindings,[
            "Only source MSB collision filters13/14/15 are converted. Authored H/L geometry and initial enable state are preserved; no global kill-height cutoff is added.",
            "Source event-controlled death-boundary changes are not ported. Firelink event11020352 disables h8100B2 after source flag710; Burg event11010790 temporarily disables h7004B1 during its source cutscene. Those unported transitions retain the authored initial state.",
            "Native fall camera, death, respawn and save behavior require gameplay validation with a copied save. No checkpoint, quest flag, player-solid floor, or source asset is changed by this stage."]));
    }

    // Replacement-mode story variants share the emitted Havok archives but have
    // independent MSBs. Add the owned fall records to each without renaming or
    // mutating its existing collision/actor/event records.
    internal static void Mirror(MSBB variant,Result result) {
        var names=variant.Parts.GetEntries().Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        var models=variant.Models.GetEntries().Select(m=>m.Name).ToHashSet(StringComparer.Ordinal);
        Require(result.Bodies.All(b=>!names.Contains(b.Part.Name)&&!models.Contains(b.Model)),"variant already occupies an imported death-boundary name");
        foreach(var body in result.Bodies){variant.Models.Collisions.Add((MSBB.Model.Collision)body.Registration.DeepCopy());variant.Parts.Collisions.Add((MSBB.Part.Collision)body.Part.DeepCopy());}
    }
}
