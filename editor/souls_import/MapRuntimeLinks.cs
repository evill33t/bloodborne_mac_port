// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;
using System.Numerics;

// Native collision-triggered map loading, with no teleports or runtime hooks.
internal static class MapRuntimeLinks {
    internal record Link(string SourceConnection,string SourceCollision,string TargetCollision,string TargetMap);
    internal record Result(int RemovedNativeLinks,IReadOnlyList<Link> Links,IReadOnlyList<string> Unavailable);
    internal static MSBB.Part.Collision[] MirrorCollisionBindings(MSBB variant,IReadOnlyList<MSBB.Part.Collision> slots) {
        // Native variants retain their own part/event names and references. The
        // model identifies the shared Havok body and therefore its import binding.
        var bodies=slots.ToDictionary(p=>p.ModelName,StringComparer.Ordinal);
        foreach(var col in variant.Parts.Collisions)if(bodies.TryGetValue(col.ModelName,out var imported)) {
            col.Position=imported.Position;col.Rotation=imported.Rotation;col.Scale=imported.Scale;col.DisableStart=imported.DisableStart;
            col.HitFilterID=imported.HitFilterID;col.MapNameID=imported.MapNameID;
            Array.Copy(imported.DrawGroups,col.DrawGroups,col.DrawGroups.Length);Array.Copy(imported.DispGroups,col.DispGroups,col.DispGroups.Length);
            col.GparamConfigStruct=imported.GparamConfigStruct.DeepCopy();col.SceneGparamConfigStruct=imported.SceneGparamConfigStruct.DeepCopy();
        }
        return slots.Select(slot=>variant.Parts.Collisions.FirstOrDefault(p=>p.ModelName==slot.ModelName)??throw new InvalidDataException("Native variant lacks shared collision body: "+slot.ModelName)).ToArray();
    }
    internal static Result Apply(MSBB map,MSB1 source,IReadOnlyList<MapCollisionBindings.Chunk> chunks,
        IReadOnlyList<MSBB.Part.Collision> slots,IReadOnlyDictionary<string,string> destinations) {
        if(chunks.Count!=slots.Count)throw new InvalidDataException("Connection collision binding count differs");
        var template=map.Parts.ConnectCollisions.FirstOrDefault();
        int removed=map.Parts.ConnectCollisions.Count;map.Parts.ConnectCollisions.Clear();
        var links=new List<Link>();var unavailable=new List<string>();
        var occupied=map.Parts.GetEntries().Select(p=>p.Name).ToHashSet(StringComparer.Ordinal);
        foreach(var connection in source.Parts.ConnectCollisions) {
            var matches=destinations.Where(d=>new MapConnections.MapPattern(connection.MapID.Select(b=>(int)b)).Matches(d.Key)).ToArray();
            if(matches.Length==0)continue;
            if(matches.Length!=1)throw new InvalidDataException("Ambiguous runtime map connection: "+connection.Name);
            var target=MapConnections.NativeBase(matches[0].Value);
            _=new MapConnections.SourceIdentity("BB",target);
            var indices=Enumerable.Range(0,chunks.Count).Where(i=>chunks[i].SourceName==connection.CollisionName).ToArray();
            if(indices.Length==0){unavailable.Add(connection.Name+": source collision is not player-solid");continue;}
            foreach(int i in indices) {
                var slot=slots[i];int suffix=1;string name;
                do{name=slot.ModelName+"_"+(suffix++).ToString("D4");}while(!occupied.Add(name));
                var part=template==null?new MSBB.Part.ConnectCollision{ResidentFXParamID=-1,LodParamID=255}:(MSBB.Part.ConnectCollision)template.DeepCopy();
                part.Name=name;part.ModelName=slot.ModelName;part.CollisionName=slot.Name;
                part.Position=slot.Position;part.Rotation=slot.Rotation;part.Scale=Vector3.One;part.EntityID=-1;
                var id=target[1..].Split('_').Select(byte.Parse).ToArray();Array.Copy(id,part.MapID,4);
                Array.Clear(part.DrawGroups);Array.Copy(connection.DrawGroups,part.DrawGroups,connection.DrawGroups.Length);
                Array.Clear(part.DispGroups);Array.Copy(connection.DispGroups,part.DispGroups,connection.DispGroups.Length);Array.Clear(part.BackreadGroups);
                map.Parts.ConnectCollisions.Add(part);links.Add(new(connection.Name,connection.CollisionName,slot.Name,target));
            }
        }
        return new(removed,links,unavailable);
    }
}
