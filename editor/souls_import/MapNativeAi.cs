// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

// Native character behavior also needs its map-local AI bytecode and registry.
// Keep existing destination scripts; append only the chosen native rig's goals.
internal static class MapNativeAi {
    static string Leaf(string path)=>path.Replace('\\','/').Split('/').Last();
    internal static IReadOnlyDictionary<string,byte[]> Fallbacks(byte[] bytes,Func<string,byte[]?>? readExisting=null) {
        var bank=BND4.Read(bytes);var files=new Dictionary<string,byte[]>(StringComparer.Ordinal);
        var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        // New areas load their map bank after some actors request loose goals.
        // Native 1eefe70 reuses those cached file objects without filling an
        // empty payload. Supply the same native bytecode to that earlier path.
        ReadOnlySpan<byte> header=Convert.FromHexString("1B4C756150010408040608090908B6099368E7F57D41");
        foreach(var file in bank.Files) {
            string name=Leaf(file.Name);
            if(!name.EndsWith(".lua",StringComparison.OrdinalIgnoreCase))continue;
            if(!name.EndsWith(".lua",StringComparison.Ordinal)||name.Length is <5 or >128||
                name[..^4].Any(c=>!(c is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '_'))||
                file.Name.Any(char.IsControl)||file.Name.Replace('\\','/').Split('/').Any(p=>p is "." or "..")||
                file.ID is <1000 or >=1000000||!names.Add(name))
                throw new InvalidDataException("Unsafe or repeated native AI fallback: "+name);
            if(file.Bytes.Length<=header.Length||!file.Bytes.Span[..header.Length].SequenceEqual(header))
                throw new InvalidDataException("Unsupported native AI bytecode: "+name);
            string path="script_interroot/ai/out/"+name;
            var previous=readExisting?.Invoke(path);
            if(previous!=null&&!previous.AsSpan().SequenceEqual(file.Bytes.Span))
                throw new InvalidDataException("Native AI fallback conflicts with an existing file: "+name);
            files.Add(path,file.Bytes.ToArray());
        }
        if(files.Count==0)throw new InvalidDataException("Native AI bank contains no bytecode fallbacks");
        return files;
    }
    internal static byte[] Merge(byte[] destination,byte[] donor,int model=2610,IReadOnlyCollection<int>? requiredGoals=null) {
        if(model is <1000 or >8999)throw new ArgumentOutOfRangeException(nameof(model));
        var target=BND4.Read(destination);var source=BND4.Read(donor);
        BinderFile Entry(BND4 binder,string suffix)=>binder.Files.Single(f=>Leaf(f.Name).EndsWith(suffix,StringComparison.Ordinal));
        var infoEntry=Entry(target,".luainfo");var info=LUAINFO.Read(infoEntry.Bytes);var original=LUAINFO.Read(Entry(source,".luainfo").Bytes);
        if(info.BigEndian!=original.BigEndian||info.LongFormat!=original.LongFormat)throw new InvalidDataException("Incompatible native AI goal registries");
        int[] dependencies=requiredGoals?.ToArray()??[];
        if(dependencies.Length>32||dependencies.Any(id=>id is <0 or >899999)||dependencies.Distinct().Count()!=dependencies.Length)
            throw new InvalidDataException("Invalid native AI goal dependencies");
        // A shared logic ID does not share the character's numeric prefix.
        // Import both its registry contract and its actual shipped bytecode.
        string[] prefixes=[model.ToString(System.Globalization.CultureInfo.InvariantCulture),..dependencies.Select(id=>id.ToString("D6",System.Globalization.CultureInfo.InvariantCulture)+"_")];
        foreach(int id in dependencies)if(original.Goals.Count(g=>g.ID==id)!=1||!source.Files.Any(f=>Leaf(f.Name).StartsWith(id.ToString("D6",System.Globalization.CultureInfo.InvariantCulture)+"_",StringComparison.Ordinal)&&Leaf(f.Name).EndsWith(".lua",StringComparison.Ordinal)))
            throw new InvalidDataException("Native AI goal dependency is unavailable: "+id);
        var ids=target.Files.Select(f=>f.ID).ToHashSet();int next=1000;
        foreach(var f in source.Files.Where(f=>prefixes.Any(prefix=>Leaf(f.Name).StartsWith(prefix,StringComparison.Ordinal))&&Leaf(f.Name).EndsWith(".lua",StringComparison.Ordinal))) {
            var existing=target.Files.SingleOrDefault(t=>Leaf(t.Name)==Leaf(f.Name));
            if(existing!=null){if(!existing.Bytes.Span.SequenceEqual(f.Bytes.Span))throw new InvalidDataException("Conflicting native AI script: "+Leaf(f.Name));continue;}
            while(ids.Contains(next))next++;ids.Add(next);
            target.Files.Add(new BinderFile(f.Flags,next++,f.Name,f.Bytes.ToArray()));
        }
        foreach(var goal in original.Goals.Where(g=>g.ID>=model*100&&g.ID<model*100+100||dependencies.Contains(g.ID))) {
            var existing=info.Goals.SingleOrDefault(g=>g.ID==goal.ID&&g.Name==goal.Name);
            if(existing!=null) {
                if(existing.BattleInterrupt!=goal.BattleInterrupt||existing.LogicInterrupt!=goal.LogicInterrupt||existing.LogicInterruptName!=goal.LogicInterruptName)throw new InvalidDataException("Conflicting native AI goal: "+goal.Name);
            } else {
                if(info.Goals.Any(g=>g.Name==goal.Name||dependencies.Contains(goal.ID)&&g.ID==goal.ID))throw new InvalidDataException("Conflicting native AI goal identity: "+goal.Name);
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
