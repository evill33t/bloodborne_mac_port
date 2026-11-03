// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;using System.Text;
internal static class GoldenGolemAi {
    internal static readonly int[] SourceGoals=[271100];
    internal static int Goal(int source){int index=Array.IndexOf(SourceGoals,source);return index>=0?883200+index:throw new InvalidDataException("Unknown Golden golem battle goal");}
    static string Leaf(string p)=>Path.GetFileName(p.Replace('\\','/'));
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Golden golem source AI: "+why);}
    internal static byte[] Build(byte[] destination,Func<string,byte[]?> source,Func<string,byte[]> native) {
        var sourceMap=BND3.Read(source("script/m12_00_00_00.luabnd.dcx")!);var common=BND3.Read(source("script/aicommon.luabnd.dcx")!);var nativeCommon=BND4.Read(native("script/aicommon.luabnd.dcx"));
        Dictionary<string,double> Enums(IEnumerable<BinderFile> files) {var values=new Dictionary<string,double>(StringComparer.Ordinal);foreach(string file in new[]{"ai_define.lua","goal_list.lua","npc_subgoals.lua"})foreach(var (key,value) in Lua50.Literals(Lua50.Read(files.Single(f=>Leaf(f.Name)==file).Bytes.ToArray())))Need(values.TryAdd(key,value)||values[key]==value,"ambiguous enum");return values;}
        var sourceEnums=Enums(common.Files);var nativeEnums=Enums(nativeCommon.Files);
        var helpers=new Dictionary<string,Lua50.Export>(StringComparer.Ordinal);foreach(string file in new[]{"common_battle_func.lua","common_func2.lua","common_func.lua"})foreach(var entry in common.Files.Where(f=>Leaf(f.Name)==file))foreach(var (key,value) in Lua50.Exports(Lua50.Read(entry.Bytes.ToArray())))Need(helpers.TryAdd(key,value),"ambiguous helper "+key);
        destination=MapNativeAi.Merge(destination,native("script/aicommon.luabnd.dcx"),7040,[10000]);var target=BND4.Read(destination);
        var infoFile=target.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal));var info=LUAINFO.Read(infoFile.Bytes);
        var namesFile=target.Files.Single(f=>Leaf(f.Name).EndsWith(".luagnl",StringComparison.Ordinal));var globals=LUAGNL.Read(namesFile.Bytes);var known=globals.Globals.ToHashSet(StringComparer.Ordinal);
        foreach(int sourceGoal in SourceGoals) {
            int goal=Goal(sourceGoal);string ownedPrefix="DS1_Golden_"+sourceGoal+"_";var entry=sourceMap.Files.Single(f=>Leaf(f.Name)==sourceGoal.ToString("D6")+"_battle.lua");var root=Lua50.Read(entry.Bytes.ToArray());
            var selected=new Dictionary<string,Lua50.Export>(StringComparer.Ordinal);void Dependencies(Lua50.Function f){foreach(string name in Lua50.Globals(f))if(helpers.TryGetValue(name,out var helper)&&selected.TryAdd(name,helper))Dependencies(helper.Function);}
            Dependencies(root);Need(selected.Count<=128,"helper closure exceeds budget");if(selected.Count>0)Lua50.PrependFunctions(root,selected);
            var goalSymbol=Lua50.Globals(root).Single(n=>n.StartsWith("GOAL_",StringComparison.Ordinal)&&sourceEnums.GetValueOrDefault(n)==sourceGoal);
            string oldName=Lua50.Exports(root).Keys.Single(n=>!selected.ContainsKey(n)&&n.EndsWith("Battle_Activate",StringComparison.Ordinal))[..^9];string name=ownedPrefix+oldName;
            var owned=Lua50.Globals(root,true).Concat(selected.Keys).Append(oldName).Concat(new[]{"Tbl","atkAfterOddsTbl","True","combRunDist"}).ToHashSet(StringComparer.Ordinal);
            var enumValues=new Dictionary<string,double>{{goalSymbol,goal}};
            foreach(string global in Lua50.Globals(root)) {
                if(owned.Contains(global)||global is "REGISTER_GOAL" or "REGISTER_GOAL_NO_UPDATE"||global==goalSymbol)continue;
                Need(sourceEnums.TryGetValue(global,out double value)&&nativeEnums.TryGetValue(global,out _),"unverified enum/API global "+global);
                Need(value==nativeEnums[global],"source/native enum mismatch "+global);enumValues[global]=value;
            }
            var strings=Lua50.All(root).SelectMany(f=>f.Constants).OfType<byte[]>().Where(b=>b.All(c=>c<128)).Select(Encoding.ASCII.GetString).ToHashSet(StringComparer.Ordinal);var rename=owned.Where(strings.Contains).ToDictionary(n=>n,n=>ownedPrefix+n,StringComparer.Ordinal);
            Lua50.RelocateIfCallbacks(root,rename,new Dictionary<int,int>{{sourceGoal,goal}},nativeCommon.Files.Single(f=>Leaf(f.Name)=="if.lua").Bytes.Span);
            Need(!info.Goals.Any(g=>g.ID==goal||g.Name==name),"owned battle goal occupied");int id=1000;while(target.Files.Any(f=>f.ID==id))id++;
            target.Files.Add(new(entry.Flags,id,@"N:\SPRJ\data\INTERROOT_ps4\script\ai\out\"+goal+"_battle.lua",Lua50.Rewrite(Lua50.Write(root),rename,enumValues)));
            info.Goals.Add(new(goal,name,true,false,null));foreach(string global in rename.Values.Order(StringComparer.Ordinal))if(known.Add(global))globals.Globals.Add(global);
        }
        infoFile.Bytes=info.Write();namesFile.Bytes=globals.Write();target.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));return target.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
