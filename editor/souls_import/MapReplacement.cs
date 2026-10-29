// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

internal static class MapReplacement {
    internal static int SelectSky(MSBB map,string model) {
        var selected=map.Parts.MapPieces.FirstOrDefault(p=>p.ModelName==model)??throw new InvalidDataException("Native sky unavailable: "+model);
        // Original time-of-day events can disable the selected sky by EntityID.
        // A separate visual instance stays independent of those native scripts.
        var visual=selected.EntityID!=-1?(MSBB.Part.MapPiece)selected.DeepCopy():null;
        var nextInstance=visual==null?0:checked(map.Parts.GetEntries().Max(p=>p.InstanceID)+1);
        // All-areas visibility must not combine day/evening/night skies or the
        // original city's distant building meshes with the imported level.
        // Alternate skies can have IDs used by EMEVD even when no MSB event
        // references them. Preserve their records while neutralizing geometry.
        var referenced=new HashSet<string>();
        foreach(var e in map.Events.GetEntries()) {
            if(e.PartName!=null)referenced.Add(e.PartName);
            if(e is MSBB.Event.Treasure t&&t.TreasurePartName!=null)referenced.Add(t.TreasurePartName);
            if(e is MSBB.Event.Generator g)foreach(var name in g.SpawnPartNames)if(name!=null)referenced.Add(name);
            if(e is MSBB.Event.ObjAct a&&a.ObjActPartName!=null)referenced.Add(a.ObjActPartName);
            if(e is MSBB.Event.PlatoonInfo group)foreach(var name in group.GroupPartsNames)if(name!=null)referenced.Add(name);
        }
        foreach(var p in map.Parts.GetEntries()) {
            var name=p switch {MSBB.Part.ObjectBase o=>o.CollisionName,MSBB.Part.EnemyBase e=>e.CollisionName,_=>null};
            if(name!=null)referenced.Add(name);
        }
        var removed=map.Parts.MapPieces.RemoveAll(p=>{
            if(!p.ModelName.StartsWith("m9",StringComparison.Ordinal)||(p.ModelName==model&&visual==null))return false;
            if(p.EntityID==-1&&!referenced.Contains(p.Name))return true;
            p.Scale=System.Numerics.Vector3.Zero;
            Array.Fill(p.DrawGroups,0u);Array.Fill(p.BackreadGroups,0u);Array.Fill(p.DispGroups,0u);
            return false;
        });
        if(visual!=null) {
            var stem=selected.Name+"_import_day";var name=stem;int suffix=1;
            var names=map.Parts.GetEntries().Select(p=>p.Name).ToHashSet();
            while(names.Contains(name))name=stem+"_"+suffix++;
            visual.Name=name;visual.EntityID=-1;visual.InstanceID=nextInstance;
            Array.Fill(visual.DrawGroups,uint.MaxValue);Array.Fill(visual.BackreadGroups,uint.MaxValue);Array.Fill(visual.DispGroups,0u);
            map.Parts.MapPieces.Add(visual);
        }
        var used=map.Parts.GetEntries().Select(p=>p.ModelName).ToHashSet();
        map.Models.MapPieces.RemoveAll(m=>!used.Contains(m.Name));
        return removed;
    }

    internal static int ParkActors(MSBB map) {
        // Dummy records preserve names/IDs used by MSB events without running
        // the old Yharnam population over a level whose floor has been replaced.
        int count = map.Parts.Enemies.Count;
        foreach (var enemy in map.Parts.Enemies) {
            var dummy = new MSBB.Part.DummyEnemy();
            foreach (var property in typeof(MSBB.Part.EnemyBase).GetProperties())
                if (property.CanRead && property.CanWrite) property.SetValue(dummy, property.GetValue(enemy));
            map.Parts.DummyEnemies.Add(dummy);
        }
        map.Parts.Enemies.Clear();
        return count;
    }

    // Keeping all 4,056 Yharnam pieces and appending Majula caused the game to
    // suppress the scenery. Retain sky, script IDs and referenced records only.
    internal static int PruneScenery(MSBB map) {
        var keep = new HashSet<string>();
        foreach (var e in map.Events.GetEntries()) {
            if (e.PartName != null) keep.Add(e.PartName);
            if (e is MSBB.Event.Treasure t && t.TreasurePartName != null) keep.Add(t.TreasurePartName);
            if (e is MSBB.Event.Generator g)
                foreach (var name in g.SpawnPartNames) if (name != null) keep.Add(name);
            if (e is MSBB.Event.ObjAct a && a.ObjActPartName != null) keep.Add(a.ObjActPartName);
        }
        // Retail object/enemy loading references can point to map pieces too.
        foreach (var p in map.Parts.GetEntries()) {
            var name = p switch {
                MSBB.Part.ObjectBase o => o.CollisionName,
                MSBB.Part.EnemyBase e => e.CollisionName,
                _ => null
            };
            if (name != null) keep.Add(name);
        }
        int removed = map.Parts.MapPieces.RemoveAll(p =>
            !p.ModelName.StartsWith("m9", StringComparison.Ordinal) &&
            p.EntityID == -1 && !keep.Contains(p.Name));
        var used = map.Parts.GetEntries().Select(p => p.ModelName).ToHashSet();
        map.Models.MapPieces.RemoveAll(m => !used.Contains(m.Name));
        return removed;
    }
}
