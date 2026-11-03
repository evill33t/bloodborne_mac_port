// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Security.Cryptography;

// The Basin goal is self-contained: all fifteen prototypes have no captures.
// Only its private globals and goal identity change. Distances, attack choices,
// height tests and common-subgoal arguments remain the authored program.
internal static class HydraAi {
    internal const int Goal=883000;
    internal const string Name="DS1_Hydra_LakeBeast_m12_353001Battle";
    const string SourceGoal="GOAL_LakeBeast_m12_353001_Battle";
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Hydra AI: "+why);}
    internal static byte[] Build(byte[] destination,Func<string,byte[]?> source,Func<string,byte[]> native) {
        var bank=BND3.Read(source("script/m12_00_00_00.luabnd.dcx")??throw new FileNotFoundException("Source Basin AI unavailable"));
        var entry=bank.Files.Single(f=>Leaf(f.Name)=="353001_battle.lua");
        Need(Convert.ToHexStringLower(SHA256.HashData(entry.Bytes.Span))=="c9982c0176ba4019fd5dd1d96b1450368f182973003806fa364213e1c1348fb4","audited Basin goal changed");
        var root=Lua50.Read(entry.Bytes.ToArray());Need(Lua50.All(root).Count()==16&&Lua50.All(root).All(f=>f.Upvalues==0),"unverified prototype closure");
        var registry=LUAINFO.Read(bank.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal)).Bytes);
        var goal=registry.Goals.Single(g=>g.ID==353001);Need(goal.Name=="LakeBeast_m12_353001Battle"&&goal.BattleInterrupt&&!goal.LogicInterrupt,"source registration changed");
        var common=BND3.Read(source("script/aicommon.luabnd.dcx")??throw new FileNotFoundException("Source common AI unavailable"));
        var nativeBytes=native("script/aicommon.luabnd.dcx");var nativeCommon=BND4.Read(nativeBytes);
        Dictionary<string,double> Enums(IEnumerable<BinderFile> files) {
            var values=new Dictionary<string,double>(StringComparer.Ordinal);
            foreach(string file in new[]{"ai_define.lua","goal_list.lua"})foreach(var (key,value) in Lua50.Literals(Lua50.Read(files.Single(f=>Leaf(f.Name)==file).Bytes.ToArray())))
                Need(values.TryAdd(key,value)||values[key]==value,"ambiguous engine enum");return values;
        }
        var from=Enums(common.Files);var to=Enums(nativeCommon.Files);Need(from.GetValueOrDefault(SourceGoal)==353001,"source goal enum changed");
        var owned=Lua50.Globals(root,true).Append("LakeBeast_m12_353001Battle").ToHashSet(StringComparer.Ordinal);
        foreach(string name in Lua50.Globals(root)) {
            if(owned.Contains(name)||name is "REGISTER_GOAL" or "REGISTER_GOAL_NO_UPDATE"||name==SourceGoal)continue;
            Need(from.TryGetValue(name,out double a)&&to.TryGetValue(name,out double b)&&a==b,"unverified global "+name);
        }
        // These are the shipped native programs behind the four common goals
        // requested by this source chunk. Reject an unknown engine script ABI.
        var wrappers=new Dictionary<string,string>{
            ["attack_tunable_spin.lua"]="d1ca935e8c0807dad8ca6dbe0c9d6dff351e43947eb56cdcff96862c1d41951f",
            ["nonspinning_attack.lua"]="7023433048817af9f871865172d1ccb1ee50fc930db4c9df70b7f5d3056470aa",
            ["turn.lua"]="fdd6baa4d41485174c9507882b0866654fbaa01c8c8302afdd8bd3ea78cc1ba7",
            ["nonspinning_combo_attack.lua"]="d88f611947c7c28599f620f336b9cb18cc69e15e117a442ddb1e1990cb160113"};
        foreach(var (file,hash) in wrappers)Need(Convert.ToHexStringLower(SHA256.HashData(nativeCommon.Files.Single(f=>Leaf(f.Name)==file).Bytes.Span))==hash,"native subgoal changed: "+file);
        var strings=Lua50.All(root).SelectMany(f=>f.Constants).OfType<byte[]>().Where(b=>b.All(c=>c<128)).Select(System.Text.Encoding.ASCII.GetString).ToHashSet(StringComparer.Ordinal);
        var names=owned.Where(strings.Contains).ToDictionary(n=>n,n=>"DS1_Hydra_"+n,StringComparer.Ordinal);
        var bytes=Lua50.Rewrite(Lua50.Write(root),names,new Dictionary<string,double>{{SourceGoal,Goal}});
        destination=MapNativeAi.Merge(destination,nativeBytes,7040,[10000,11000]);
        var target=BND4.Read(destination);var infoFile=target.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal));var info=LUAINFO.Read(infoFile.Bytes);
        Need(!info.Goals.Any(g=>g.ID==Goal||g.Name==Name),"owned goal occupied");string leaf=Goal+"_battle.lua";
        Need(target.Files.All(f=>Leaf(f.Name)!=leaf),"owned script occupied");int id=1000;while(target.Files.Any(f=>f.ID==id))id++;
        target.Files.Add(new BinderFile(entry.Flags,id,@"N:\SPRJ\data\INTERROOT_ps4\script\ai\out\"+leaf,bytes));
        info.Goals.Add(new(Goal,Name,true,false,null));infoFile.Bytes=info.Write();
        var namesFile=target.Files.Single(f=>Leaf(f.Name).EndsWith(".luagnl",StringComparison.Ordinal));var globals=LUAGNL.Read(namesFile.Bytes);var existing=globals.Globals.ToHashSet(StringComparer.Ordinal);
        foreach(string name in names.Values.Order(StringComparer.Ordinal))if(existing.Add(name))globals.Globals.Add(name);
        namesFile.Bytes=globals.Write();target.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));return target.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
