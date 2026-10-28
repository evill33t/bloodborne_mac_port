// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

internal static class MapReplacement {
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
