// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;
using System.Text.RegularExpressions;

// Planning data only: this module neither modifies an MSB nor activates native streaming.
internal static class MapConnections {
    internal sealed record SourceIdentity {
        public string Profile { get; }
        public string Map { get; }
        public string Key=>Profile+":"+Map;
        public string Area=>Profile+":"+Map[..6];
        public SourceIdentity(string profile,string map) {
            if(!Regex.IsMatch(profile,@"\A[A-Z][A-Z0-9_]{0,15}\z"))throw new InvalidDataException("Invalid source profile identity");
            ValidateMap(map);Profile=profile;Map=map;
        }
    }
    internal sealed record MapPattern {
        // Use integers so JSON preserves the four components instead of base64-encoding byte[].
        public int[] Components { get; }
        public string Pattern=>"m"+string.Join('_',Components.Select(n=>n==255?"??":n.ToString("D2")));
        public MapPattern(IEnumerable<int> components) {
            Components=components.ToArray();
            if(Components.Length!=4||Components.Any(n=>n<0||n>99&&n!=255))throw new InvalidDataException("Invalid connection map ID");
        }
        public bool Matches(string map) {
            var values=MapComponents(map);return Components.Select((n,i)=>n==255||n==values[i]).All(n=>n);
        }
    }
    internal sealed record Transform(Vector3 Position,Vector3 Rotation,Vector3 Scale);
    internal sealed record Collision(string Name,string? Model,Transform Transform,uint[] DrawGroups,uint[] DisplayGroups,IReadOnlyDictionary<string,long> Values);
    internal sealed record Connection(string Name,string? Model,Transform Transform,uint[] DrawGroups,uint[] DisplayGroups,string? CollisionName,Collision? Collision,
        MapPattern Destination,SourceIdentity[] Candidates,string Resolution);
    internal sealed record RegionReference(string Kind,string Name,string Field);
    internal sealed record Region(string Name,string Kind,int? EntityID,int? UniqueID,Transform Transform,
        string Shape,IReadOnlyDictionary<string,float> Dimensions,RegionReference[] References);
    internal sealed record TravelMarker(string Kind,string Name,int? EventID,int? EntityID,string? RegionName,
        Transform? Transform,string? CollisionName,string[] UnresolvedSemantics,IReadOnlyDictionary<string,int>? Values=null);
    internal sealed record MapOffset(string Name,Vector3 Translation,float RotationY,bool ApplyToPartPositions);
    internal sealed record Dependency(string Kind,string[] Paths);
    internal sealed record Inventory(string Schema,SourceIdentity Source,Connection[] Connections,Collision[] Collisions,
        Region[] Regions,TravelMarker[] TravelMarkers,MapOffset[] MapOffsets,Dependency[] Dependencies,string[] Diagnostics,string[] Limitations);
    internal sealed record AllocationRequest(SourceIdentity Source,Vector3 SourceAnchor,Vector3 TargetAnchor,string? TargetMap=null);
    internal sealed record Allocation(SourceIdentity Source,string TargetMap,string NativeBaseMap,string AssetNamespace,Vector3 SourceAnchor,Vector3 TargetAnchor) {
        public Vector3 Translation=>TargetAnchor-SourceAnchor;
    }
    internal sealed record SharedOutput(string Path,string[] Sources);
    internal sealed record Reservations(string Schema,Allocation[] Maps,SharedOutput[] SharedOutputs,string[] Limitations);
    internal sealed record LinkProposal(SourceIdentity Source,string Connection,SourceIdentity? Destination,string? FromTargetMap,string? ToTargetMap,
        string? NativeCollision,string[] ReciprocalConnections,Vector3? TranslationDifference,string Status,bool RuntimeEnabled,string[] RemainingWork);
    internal sealed record LinkPlan(string Schema,LinkProposal[] Links,string[] Limitations);

    static readonly string[] RuntimeLimits=[
        "Inventory and target reservations do not implement or validate native map streaming or travel.",
        "Source scripts, quest conditions, loading gates and external region references have not been translated.",
        "Source map coordinates are retained; compatible coordinate frames must be verified from geometry and collision before seamless links are emitted."];
    static void ValidateMap(string map) {
        if(!Regex.IsMatch(map,@"\Am\d\d_\d\d_\d\d_\d\d\z"))throw new InvalidDataException("Invalid map identity: "+map);
    }
    static int[] MapComponents(string map){ValidateMap(map);return map[1..].Split('_').Select(int.Parse).ToArray();}
    static void Finite(Vector3 value) {
        if(!float.IsFinite(value.X)||!float.IsFinite(value.Y)||!float.IsFinite(value.Z))throw new InvalidDataException("Non-finite map transform");
    }
    static Transform Placement(Vector3 position,Vector3 rotation,Vector3 scale) {
        Finite(position);Finite(rotation);Finite(scale);return new(position,rotation,scale);
    }
    static Dictionary<string,T> Named<T>(IEnumerable<T> values,Func<T,string> name,string kind) {
        var result=new Dictionary<string,T>(StringComparer.Ordinal);
        foreach(var item in values)if(string.IsNullOrEmpty(name(item))||!result.TryAdd(name(item),item))throw new InvalidDataException("Missing or duplicate "+kind+" name");
        return result;
    }
    static Dictionary<string,float> Dimensions(MSB.Shape shape) {
        var result=new Dictionary<string,float>();
        foreach(var property in shape.GetType().GetProperties().Where(p=>p.PropertyType==typeof(float))) {
            float value=(float)property.GetValue(shape)!;
            if(!float.IsFinite(value)||value<0)throw new InvalidDataException("Invalid region shape dimension");result.Add(property.Name,value);
        }return result;
    }
    static SourceIdentity[] Catalog(SourceIdentity source,IEnumerable<SourceIdentity>? catalog)=>catalog?.Where(s=>s.Profile==source.Profile).Distinct().OrderBy(s=>s.Key,StringComparer.Ordinal).ToArray()??[];
    static Connection Link(string name,string? model,Transform transform,uint[] drawGroups,uint[] displayGroups,string? collisionName,IEnumerable<byte> mapID,
        IReadOnlyDictionary<string,Collision> collisions,SourceIdentity[] catalog,bool catalogSupplied,List<string> diagnostics) {
        var destination=new MapPattern(mapID.Select(n=>(int)n));var candidates=catalog.Where(s=>destination.Matches(s.Map)).ToArray();
        Collision? collision=null;
        if(string.IsNullOrEmpty(collisionName)||!collisions.TryGetValue(collisionName,out collision))diagnostics.Add($"Connection {name} references unavailable collision {collisionName ?? "<none>"}");
        return new(name,model,transform,drawGroups.ToArray(),displayGroups.ToArray(),collisionName,collision,destination,candidates,!catalogSupplied?"catalog-not-supplied":candidates.Length switch {0=>"unavailable",1=>"unique",_=>"ambiguous"});
    }
    static Dictionary<string,List<RegionReference>> RegionReferences<T>(IEnumerable<T> entries) where T:notnull {
        var result=new Dictionary<string,List<RegionReference>>(StringComparer.Ordinal);
        foreach(var item in entries)foreach(var property in item.GetType().GetProperties().Where(p=>p.Name is "RegionName" or "SpawnPointName"||p.Name.EndsWith("RegionNames",StringComparison.Ordinal))) {
            var names=property.GetValue(item) switch {string s=>new[]{s},IEnumerable<string> many=>many,_=>[]};
            foreach(var name in names.Where(n=>!string.IsNullOrEmpty(n))) {
                if(!result.TryGetValue(name,out var refs))result.Add(name,refs=[]);
                var itemName=(string?)item.GetType().GetProperty("Name")?.GetValue(item)??"";
                refs.Add(new(item.GetType().Name,itemName,property.Name));
            }
        }return result;
    }
    static RegionReference[] References(IReadOnlyDictionary<string,List<RegionReference>> refs,string name)=>refs.TryGetValue(name,out var values)?values.ToArray():[];
    static Inventory Finish(SourceIdentity source,Connection[] links,Collision[] collisions,Region[] regions,TravelMarker[] markers,MapOffset[] offsets,List<string> diagnostics) {
        Named(links,l=>l.Name,"connection");Named(regions,r=>r.Name,"region");
        foreach(var marker in markers.Where(m=>m.RegionName!=null&&!regions.Any(r=>r.Name==m.RegionName)))diagnostics.Add($"{marker.Kind} {marker.Name} references unavailable region {marker.RegionName}");
        return new("bbmap-connections-1",source,links,collisions,regions,markers,offsets,Dependencies(source),diagnostics.ToArray(),RuntimeLimits.ToArray());
    }
    internal static Inventory Read(byte[] bytes,SourceIdentity source,IEnumerable<SourceIdentity>? catalog=null)=>source.Profile switch {
        "DS1" or "DS1R"=>Inspect(MSB1.Read(bytes),source,catalog),
        "DS2" or "DS2S"=>Inspect(MSB2.Read(bytes),source,catalog),
        _=>throw new NotSupportedException("Connection inventory adapter unavailable for "+source.Profile)
    };
    internal static Inventory Inspect(MSB1 msb,SourceIdentity source,IEnumerable<SourceIdentity>? catalog=null) {
        if(source.Profile is not("DS1" or "DS1R"))throw new InvalidDataException("MSB1 requires a DS1 source identity");
        var diagnostics=new List<string>();var available=Catalog(source,catalog);
        var collisions=Named(msb.Parts.Collisions.Select(p=>new Collision(p.Name,p.ModelName,Placement(p.Position,p.Rotation,p.Scale),p.DrawGroups.ToArray(),p.DispGroups.ToArray(),new Dictionary<string,long>{["EntityID"]=p.EntityID,["HitFilterID"]=p.HitFilterID,["MapNameID"]=p.MapNameID,["PlayRegionID"]=p.PlayRegionID,["DisableStart"]=p.DisableStart?1:0,["DisableBonfireEntityID"]=p.DisableBonfireEntityID,["LockCamParamID1"]=p.LockCamParamID1,["LockCamParamID2"]=p.LockCamParamID2})),c=>c.Name,"collision");
        var links=msb.Parts.ConnectCollisions.Select(p=>Link(p.Name,p.ModelName,Placement(p.Position,p.Rotation,p.Scale),p.DrawGroups,p.DispGroups,p.CollisionName,p.MapID,collisions,available,catalog!=null,diagnostics)).ToArray();
        var refs=RegionReferences(msb.Events.GetEntries());
        var regions=msb.Regions.GetEntries().Select(r=>new Region(r.Name,"Region",r.EntityID,null,Placement(r.Position,r.Rotation,Vector3.One),r.Shape.GetType().Name,Dimensions(r.Shape),References(refs,r.Name))).ToArray();
        var markers=msb.Parts.Players.Select(p=>new TravelMarker("PlayerStart",p.Name,null,p.EntityID,null,Placement(p.Position,p.Rotation,p.Scale),null,["Arrival activation and source event conditions are not converted."]))
            .Concat(msb.Events.SpawnPoints.Select(e=>new TravelMarker("SpawnPoint",e.Name,e.EventID,e.EntityID,e.SpawnPointName,
                regions.FirstOrDefault(r=>r.Name==e.SpawnPointName)?.Transform,null,["Spawn point presence does not establish a travel destination or activation condition."]))).ToArray();
        var offsets=msb.Events.MapOffsets.Select(e=>{Finite(e.Position);if(!float.IsFinite(e.RotationY))throw new InvalidDataException("Non-finite map offset");return new MapOffset(e.Name,e.Position,e.RotationY,false);}).ToArray();
        return Finish(source,links,collisions.Values.ToArray(),regions,markers,offsets,diagnostics);
    }
    internal static Inventory Inspect(MSB2 msb,SourceIdentity source,IEnumerable<SourceIdentity>? catalog=null) {
        if(source.Profile is not("DS2" or "DS2S"))throw new InvalidDataException("MSB2 requires a DS2 source identity");
        var diagnostics=new List<string>();var available=Catalog(source,catalog);
        var collisions=Named(msb.Parts.Collisions.Select(p=>new Collision(p.Name,p.ModelName,Placement(p.Position,p.Rotation,p.Scale),p.DrawGroups.ToArray(),p.DispGroups.ToArray(),new Dictionary<string,long>{["HitFilterID"]=(int)p.HitFilterID,["FilterID"]=p.FilterID,["MapNameID"]=p.MapNameID,["PlayAreaID"]=p.PlayAreaID,["HitGroupID"]=p.HitGroupID,["MemoryBlockID"]=p.MemoryBlockID,["DisableInvasion"]=p.DisableInvasion})),c=>c.Name,"collision");
        var links=msb.Parts.ConnectCollisions.Select(p=>Link(p.Name,p.ModelName,Placement(p.Position,p.Rotation,p.Scale),p.DrawGroups,p.DispGroups,p.CollisionName,p.MapID,collisions,available,catalog!=null,diagnostics)).ToArray();
        var regions=msb.Regions.GetEntries().Select(r=>new Region(r.Name,r.GetType().Name,null,r.UniqueID,Placement(r.Position,r.Rotation,Vector3.One),r.Shape.GetType().Name,Dimensions(r.Shape),[])).ToArray();
        var markers=msb.Regions.StartPoints.Select(r=>new TravelMarker("StartPoint",r.Name,null,null,r.Name,Placement(r.Position,r.Rotation,Vector3.One),null,["Source start-point selection conditions are not converted."]))
            .Concat(msb.Events.Warps.Select(e=>new TravelMarker("WarpEvent",e.Name,e.EventID,null,null,Placement(e.Position,Vector3.Zero,Vector3.One),null,["DS2 Warp event destination semantics are unresolved; this is a source position only."],new Dictionary<string,int>{["UnkT00"]=e.UnkT00})))
            .Concat(msb.Parts.WarpCollisions.Select(p=>new TravelMarker("WarpCollision",p.Name,null,null,null,Placement(p.Position,p.Rotation,p.Scale),p.CollisionName,["DS2 WarpCollision destination and private offset semantics are unresolved."]))).ToArray();
        foreach(var marker in markers.Where(m=>m.Kind=="WarpCollision"&&(m.CollisionName==null||!collisions.ContainsKey(m.CollisionName))))diagnostics.Add($"WarpCollision {marker.Name} references unavailable collision {marker.CollisionName ?? "<none>"}");
        var offsets=msb.Events.MapOffsets.Select(e=>{Finite(e.Translation);return new MapOffset(e.Name,e.Translation,0,false);}).ToArray();
        return Finish(source,links,collisions.Values.ToArray(),regions,markers,offsets,diagnostics);
    }
    static Dependency[] Dependencies(SourceIdentity source) {
        string map=source.Map,area=map[..3];
        if(source.Profile is "DS1" or "DS1R")return [
            new("map",[$"map/MapStudio/{map}.msb",$"map/MapStudio/{map}.msb.dcx"]),
            new("geometry",[$"map/{map}/*.flver",$"map/{map}/*.flver.dcx","obj/*.objbnd[.dcx]"]),
            new("collision",[$"map/{map}/h{map[1..]}.hkxbhd",$"map/{map}/h{map[1..]}.hkxbdt",$"map/{map}/l{map[1..]}.hkxbhd",$"map/{map}/l{map[1..]}.hkxbdt"]),
            new("textures",[$"map/{area}/{area}_*.tpfbhd",$"map/{area}/{area}_*.tpfbdt",$"map/{area}/{area}_9999.tpf[.dcx]"]),
            new("navigation",[$"map/{map}/{map}.nvmbnd.dcx",$"map/{map}/{map}.mcp",$"map/{map}/{map}.mcg"]),
            new("scripts",[$"event/{map}.emevd[.dcx]",$"script/{map}.luabnd[.dcx]"])];
        return [new("map",[$"map/{map}/{map}.msb"]),new("geometry",[$"model/map/{map}.mapbhd",$"model/map/{map}.mapbdt","model/obj/*.bnd"]),
            new("collision",[$"model/map/h{map[1..]}.hkxbhd",$"model/map/h{map[1..]}.hkxbdt",$"model/map/l{map[1..]}.hkxbhd",$"model/map/l{map[1..]}.hkxbdt"]),
            new("textures",[$"model/map/t{map[1..]}.tpfbhd",$"model/map/t{map[1..]}.tpfbdt"])];
    }

    internal static string NativeBase(string map){ValidateMap(map);return map[..6]+"_00_00";}
    static Allocation Allocate(AllocationRequest request,string target) {
        Finite(request.SourceAnchor);Finite(request.TargetAnchor);ValidateMap(target);
        return new(request.Source,target,NativeBase(target),"imp_"+request.Source.Profile.ToLowerInvariant()+"_"+request.Source.Map,request.SourceAnchor,request.TargetAnchor);
    }
    internal static Reservations Reserve(IEnumerable<AllocationRequest> requests,IEnumerable<string> approvedTargetSlots,IEnumerable<Allocation>? previous=null) {
        var requested=Named(requests,r=>r.Source.Key,"source allocation");
        var slots=approvedTargetSlots.Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();foreach(var slot in slots)ValidateMap(slot);
        var allocations=Named(previous??[],a=>a.Source.Key,"previous allocation");var used=new HashSet<string>(StringComparer.Ordinal);
        foreach(var old in allocations.Values) {
            var canonical=Allocate(new(old.Source,old.SourceAnchor,old.TargetAnchor),old.TargetMap);
            if(old!=canonical)throw new InvalidDataException("Previous allocation metadata is inconsistent");
            if(!slots.Contains(old.TargetMap,StringComparer.Ordinal)||!used.Add(old.NativeBaseMap))throw new InvalidDataException("Previous allocation is unapproved or shares native map assets");
        }
        // Reserve explicit choices first so automatic assignments cannot steal an approved choice.
        foreach(var request in requested.Values.OrderBy(r=>r.TargetMap==null?1:0).ThenBy(r=>r.Source.Key,StringComparer.Ordinal)) {
            if(allocations.TryGetValue(request.Source.Key,out var old)) {
                if(old.SourceAnchor!=request.SourceAnchor||old.TargetAnchor!=request.TargetAnchor||request.TargetMap!=null&&request.TargetMap!=old.TargetMap)throw new InvalidDataException("An existing source allocation cannot be silently moved");continue;
            }
            string target=request.TargetMap??slots.FirstOrDefault(s=>!used.Contains(NativeBase(s)))??throw new InvalidDataException("Insufficient approved native target map slots");
            if(!slots.Contains(target,StringComparer.Ordinal)||!used.Add(NativeBase(target)))throw new InvalidDataException("Target map is unapproved or shares native map assets");
            allocations.Add(request.Source.Key,Allocate(request,target));
        }
        var maps=allocations.Values.OrderBy(a=>a.Source.Key,StringComparer.Ordinal).ToArray();var shared=new List<SharedOutput>();
        foreach(var group in maps.GroupBy(a=>a.TargetMap[..3]).OrderBy(g=>g.Key,StringComparer.Ordinal))foreach(var extension in new[]{"bhd","bdt"})shared.Add(new($"map/{group.Key}/{group.Key}_0003.tpf{extension}",group.Select(a=>a.Source.Key).ToArray()));
        foreach(var path in new[]{"param/gameparam/gameparam.parambnd.dcx","mtd/allmaterialbnd.mtdbnd.dcx","msg/*/item.msgbnd.dcx"})shared.Add(new(path,maps.Select(a=>a.Source.Key).ToArray()));
        return new("bbmap-target-reservations-1",maps,shared.ToArray(),[
            "Targets must be caller-approved native map slots; reserving a name does not create a loadable map.",
            "Variants in one mAA_BB family share native geometry/collision and cannot be allocated independently.",
            "Maps sharing texture, parameter, material or message outputs require one composed export; independent whole-binder mods overwrite one another."]);
    }
    internal static string CollisionKey(SourceIdentity source,string collision)=>source.Key+"/collision/"+collision;
    internal static LinkPlan Plan(IEnumerable<Inventory> inventories,Reservations reservations,IReadOnlyDictionary<string,string>? nativeCollisions=null) {
        var sources=Named(inventories,i=>i.Source.Key,"source inventory");var targets=Named(reservations.Maps,a=>a.Source.Key,"target allocation");var links=new List<LinkProposal>();
        foreach(var inventory in sources.Values.OrderBy(i=>i.Source.Key,StringComparer.Ordinal))foreach(var link in inventory.Connections) {
            SourceIdentity? destination=link.Candidates.Length==1?link.Candidates[0]:null;
            targets.TryGetValue(inventory.Source.Key,out var from);Allocation? to=null;if(destination!=null)targets.TryGetValue(destination.Key,out to);
            string? collision=null;if(link.Collision!=null)nativeCollisions?.TryGetValue(CollisionKey(inventory.Source,link.Collision.Name),out collision);
            var reciprocal=destination!=null&&sources.TryGetValue(destination.Key,out var other)?other.Connections.Where(c=>c.Candidates.Any(s=>s.Key==inventory.Source.Key)).Select(c=>c.Name).ToArray():[];
            string status=link.Resolution!="unique"?link.Resolution:from==null?"source-not-allocated":to==null?"destination-not-allocated":!sources.ContainsKey(destination!.Key)?"destination-not-inventoried":link.Collision==null?"source-collision-unresolved":string.IsNullOrEmpty(collision)?"native-collision-unmapped":"planned-needs-export-and-runtime-validation";
            links.Add(new(inventory.Source,link.Name,destination,from?.TargetMap,to?.TargetMap,collision,reciprocal,from!=null&&to!=null?to.Translation-from.Translation:null,status,false,[
                "Compose both target map geometry, high/low collision, models, groups and shared binders.",
                "Verify source coordinate relationship and native map offsets or implement explicit region-to-spawn warps.",
                "Translate loading/event gates, then test arrival, return travel, save reload and unloaded-map behavior in game."]));
        }return new("bbmap-link-plan-1",links.ToArray(),RuntimeLimits.ToArray());
    }
}
