// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// Native character behavior also needs its map-local AI bytecode and registry.
// Keep existing destination scripts; append only the chosen native rig's goals.
internal static class MapNativeAi {
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    internal static byte[] Merge(byte[] destination,byte[] donor) {
        var target=BND4.Read(destination);var source=BND4.Read(donor);
        var ids=target.Files.Select(f=>f.ID).ToHashSet();int next=1000;
        foreach(var f in source.Files.Where(f=>Leaf(f.Name).StartsWith("2610",StringComparison.Ordinal)&&Leaf(f.Name).EndsWith(".lua",StringComparison.Ordinal))) {
            var existing=target.Files.SingleOrDefault(t=>Leaf(t.Name)==Leaf(f.Name));
            if(existing!=null){if(!existing.Bytes.Span.SequenceEqual(f.Bytes.Span))throw new InvalidDataException("Conflicting native AI script: "+Leaf(f.Name));continue;}
            while(ids.Contains(next))next++;ids.Add(next);
            target.Files.Add(new BinderFile(f.Flags,next++,f.Name,f.Bytes.ToArray()));
        }
        BinderFile Entry(BND4 binder,string suffix)=>binder.Files.Single(f=>Leaf(f.Name).EndsWith(suffix,StringComparison.Ordinal));
        var infoEntry=Entry(target,".luainfo");var info=LUAINFO.Read(infoEntry.Bytes);var original=LUAINFO.Read(Entry(source,".luainfo").Bytes);
        if(info.BigEndian!=original.BigEndian||info.LongFormat!=original.LongFormat)throw new InvalidDataException("Incompatible native AI goal registries");
        foreach(var goal in original.Goals.Where(g=>g.ID is >=261000 and <261100)) {
            var existing=info.Goals.SingleOrDefault(g=>g.ID==goal.ID&&g.Name==goal.Name);
            if(existing!=null) {
                if(existing.BattleInterrupt!=goal.BattleInterrupt||existing.LogicInterrupt!=goal.LogicInterrupt||existing.LogicInterruptName!=goal.LogicInterruptName)throw new InvalidDataException("Conflicting native AI goal: "+goal.Name);
            } else {
                if(info.Goals.Any(g=>g.Name==goal.Name))throw new InvalidDataException("Conflicting native AI goal identity: "+goal.Name);
                info.Goals.Add(new LUAINFO.Goal(goal.ID,goal.Name,goal.BattleInterrupt,goal.LogicInterrupt,goal.LogicInterruptName));
            }
        }
        infoEntry.Bytes=info.Write();
        var namesEntry=Entry(target,".luagnl");var names=LUAGNL.Read(namesEntry.Bytes);var sourceNames=LUAGNL.Read(Entry(source,".luagnl").Bytes);
        if(names.BigEndian!=sourceNames.BigEndian||names.LongFormat!=sourceNames.LongFormat)throw new InvalidDataException("Incompatible native AI global-name tables");
        var globals=names.Globals.ToHashSet(StringComparer.Ordinal);foreach(string name in sourceNames.Globals)if(globals.Add(name))names.Globals.Add(name);
        namesEntry.Bytes=names.Write();target.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));return target.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
