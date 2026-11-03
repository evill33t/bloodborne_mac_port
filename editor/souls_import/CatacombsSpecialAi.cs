// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Text;

internal static class CatacombsSpecialAi {
    internal static readonly IReadOnlyDictionary<int,int> Goals=new Dictionary<int,int>{{350100,884600},{230000,884700},{279002,884800},{330000,884900},{330001,884901},{330002,884902}};
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Catacombs special AI: "+why);}
    internal static byte[] Build(byte[] destination,Func<string,byte[]?> source,Func<string,byte[]> native) {
        var area=BND3.Read(source("script/m13_00_00_00.luabnd.dcx")!);var common=BND3.Read(source("script/aicommon.luabnd.dcx")!);var nativeCommon=BND4.Read(native("script/aicommon.luabnd.dcx"));
        Dictionary<string,double> Enums(IEnumerable<BinderFile> files){var d=new Dictionary<string,double>(StringComparer.Ordinal);foreach(string file in new[]{"ai_define.lua","goal_list.lua","npc_subgoals.lua"})foreach(var (n,v) in Lua50.Literals(Lua50.Read(files.Single(f=>Leaf(f.Name)==file).Bytes.ToArray())))Need(d.TryAdd(n,v)||d[n]==v,"ambiguous engine enum");return d;}
        var sourceEnums=Enums(common.Files);var nativeEnums=Enums(nativeCommon.Files);var helpers=new Dictionary<string,Lua50.Export>(StringComparer.Ordinal);
        foreach(string file in new[]{"common_battle_func.lua","common_func2.lua","common_func.lua"})foreach(var entry in common.Files.Where(f=>Leaf(f.Name)==file))foreach(var (name,value) in Lua50.Exports(Lua50.Read(entry.Bytes.ToArray())))Need(helpers.TryAdd(name,value),"ambiguous helper "+name);
        var sourceInfo=LUAINFO.Read(area.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo")).Bytes);var commonInfo=LUAINFO.Read(common.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo")).Bytes);
        LUAINFO.Goal Descriptor(int id){var local=sourceInfo.Goals.Where(g=>g.ID==id).ToArray();return local.Length==1?local[0]:commonInfo.Goals.Single(g=>g.ID==id);}
        var target=BND4.Read(MapNativeAi.Merge(destination,native("script/aicommon.luabnd.dcx"),7040,[10000]));var infoFile=target.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo"));var info=LUAINFO.Read(infoFile.Bytes);var globalsFile=target.Files.Single(f=>Leaf(f.Name).EndsWith(".luagnl"));var globals=LUAGNL.Read(globalsFile.Bytes);var known=globals.Globals.ToHashSet(StringComparer.Ordinal);
        foreach(int id in new[]{350100,230000,279002,330000}) {
            string leaf=id.ToString("D6")+"_battle.lua";var local=area.Files.Where(f=>Leaf(f.Name)==leaf).ToArray();var file=local.Length==1?local[0]:common.Files.Single(f=>Leaf(f.Name)==leaf);var root=Lua50.Read(file.Bytes.ToArray());string prefix="DS1_CatSpecial_"+id+"_";
            var closure=new Dictionary<string,Lua50.Export>(StringComparer.Ordinal);void Dependencies(Lua50.Function f){foreach(string name in Lua50.Globals(f))if(helpers.TryGetValue(name,out var entry)&&closure.TryAdd(name,entry))Dependencies(entry.Function);}Dependencies(root);Need(closure.Count<=128,"helper budget");if(closure.Count>0)Lua50.PrependFunctions(root,closure);
            var invoked=Lua50.Globals(root).Where(n=>n.StartsWith("GOAL_",StringComparison.Ordinal)&&sourceEnums.TryGetValue(n,out double v)&&Goals.ContainsKey((int)v)).Select(n=>(Name:n,ID:(int)sourceEnums[n])).Distinct().ToArray();Need(invoked.Any(g=>g.ID==id),"source root goal not registered");
            var descriptors=invoked.Select(g=>Descriptor(g.ID)).DistinctBy(g=>g.ID).ToArray();var owned=Lua50.Globals(root,true).Concat(closure.Keys).Concat(descriptors.Select(g=>g.Name)).Concat(new[]{"Tbl","atkAfterOddsTbl","True","combRunDist","GetWellSpace_Odds","oddsSide"}).ToHashSet(StringComparer.Ordinal);var enums=invoked.ToDictionary(g=>g.Name,g=>(double)Goals[g.ID],StringComparer.Ordinal);
            foreach(string global in Lua50.Globals(root)) {if(owned.Contains(global)||enums.ContainsKey(global)||global is "REGISTER_GOAL" or "REGISTER_GOAL_NO_UPDATE" or "REGISTER_GOAL_NO_INTERUPT")continue;Need(sourceEnums.TryGetValue(global,out double value)&&nativeEnums.TryGetValue(global,out double nativeValue)&&value==nativeValue,"unverified engine global "+global);enums[global]=value;}
            var strings=Lua50.All(root).SelectMany(f=>f.Constants).OfType<byte[]>().Where(b=>b.All(c=>c<128)).Select(Encoding.ASCII.GetString).ToHashSet(StringComparer.Ordinal);var rename=owned.Where(strings.Contains).ToDictionary(n=>n,n=>prefix+n,StringComparer.Ordinal);int binder=1000;while(target.Files.Any(f=>f.ID==binder))binder++;
            target.Files.Add(new(file.Flags,binder,@"N:\SPRJ\data\INTERROOT_ps4\script\ai\out\"+Goals[id]+"_battle.lua",Lua50.Rewrite(Lua50.Write(root),rename,enums)));
            foreach(var goal in descriptors){int targetGoal=Goals[goal.ID];string name=prefix+goal.Name;Need(!info.Goals.Any(g=>g.ID==targetGoal||g.Name==name),"private goal occupied");info.Goals.Add(new(targetGoal,name,goal.BattleInterrupt,goal.LogicInterrupt,goal.LogicInterruptName==null?null:prefix+goal.LogicInterruptName));}
            foreach(string global in rename.Values.Order(StringComparer.Ordinal))if(known.Add(global))globals.Globals.Add(global);
        }
        infoFile.Bytes=info.Write();globalsFile.Bytes=globals.Write();target.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));return target.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
