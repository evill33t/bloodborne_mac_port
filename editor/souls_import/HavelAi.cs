// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
using System.Text;

internal static class HavelAi {
    internal const string Name="DS1_Havel_Habel6580Battle";
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Havel source AI: "+why);}
    internal static byte[] Build(byte[] destination,Func<string,byte[]?> source,Func<string,byte[]> native,int leashRegion) {
        var sourceMap=BND3.Read(source("script/m10_01_00_00.luabnd.dcx")!);var common=BND3.Read(source("script/aicommon.luabnd.dcx")!);var nativeCommon=BND4.Read(native("script/aicommon.luabnd.dcx"));
        var entry=sourceMap.Files.Single(f=>Leaf(f.Name)=="006580_battle.lua");var root=Lua50.Read(entry.Bytes.ToArray());
        var helpers=new Dictionary<string,Lua50.Export>(StringComparer.Ordinal);
        foreach(string file in new[]{"common_battle_func.lua","common_func2.lua","common_func.lua"}) {
            var matches=common.Files.Where(f=>Leaf(f.Name)==file).ToArray();if(matches.Length==0)continue;
            foreach(var (key,value) in Lua50.Exports(Lua50.Read(matches.Single().Bytes.ToArray())))Need(helpers.TryAdd(key,value),"ambiguous helper "+key);
        }
        var selected=new Dictionary<string,Lua50.Export>(StringComparer.Ordinal);
        void Dependencies(Lua50.Function f){foreach(string name in Lua50.Globals(f))if(helpers.TryGetValue(name,out var helper)&&selected.TryAdd(name,helper))Dependencies(helper.Function);}
        Dependencies(root);Need(selected.Count is >0 and <=128,"invalid helper closure");Lua50.PrependFunctions(root,selected);
        Dictionary<string,double> Enums(IEnumerable<BinderFile> files) {
            var values=new Dictionary<string,double>(StringComparer.Ordinal);
            foreach(string file in new[]{"ai_define.lua","goal_list.lua","npc_subgoals.lua"})foreach(var (key,value) in Lua50.Literals(Lua50.Read(files.Single(f=>Leaf(f.Name)==file).Bytes.ToArray())))Need(values.TryAdd(key,value)||values[key]==value,"ambiguous engine enum");return values;
        }
        var sourceEnums=Enums(common.Files);var nativeEnums=Enums(nativeCommon.Files);
        const string goal="GOAL_Habel6580_Battle";Need(sourceEnums.GetValueOrDefault(goal)==6580,"source Havel goal changed");
        var owned=Lua50.Globals(root,true).Concat(selected.Keys).Append("Habel6580Battle").Concat(new[]{"Tbl","atkAfterOddsTbl","True","combRunDist"}).ToHashSet(StringComparer.Ordinal);
        var enums=new Dictionary<string,double>{{goal,HavelParameters.Think}};
        // These inputs are registered by native engine code, rather than the
        // Lua enum chunks. Native NormalL2 is trick switching; source NormalL3
        // means guarding and must use the native Guard3 input.
        var inputs=new Dictionary<string,double>{{"NPC_ATK_NormalR",0},{"NPC_ATK_LargeR",1},{"NPC_ATK_NormalL",3},{"NPC_ATK_SwitchWep",2},{"NPC_ATK_ChangeWep_R1",11},{"NPC_ATK_BackstepB",14}};
        foreach(string name in Lua50.Globals(root)) {
            if(owned.Contains(name)||name is "REGISTER_GOAL" or "REGISTER_GOAL_NO_UPDATE"||name==goal)continue;
            if(inputs.TryGetValue(name,out double input)){enums[name]=input;continue;}
            Need(sourceEnums.TryGetValue(name,out double value)&&nativeEnums.TryGetValue(name,out _),"unverified global "+name);
            double translated=nativeEnums[name];Need(value==translated,"unverified enum translation "+name+" "+value+"->"+translated);
        }
        // Source interrupt's literal11/12 are lateral step inputs, not engine
        // goal IDs or helper distances. Translate only that exported prototype.
        var export=Lua50.Exports(root);var interrupt=export.Single(p=>p.Key=="Habel6580Battle_Interupt").Value.Function;
        var steps=Lua50.Read(Lua50.Rewrite(Lua50.Write(interrupt),new Dictionary<string,string>(),new Dictionary<string,double>(),new Dictionary<double,double>{{11,15},{12,16}}));
        interrupt.Constants=steps.Constants;interrupt.Code=steps.Code;interrupt.Functions=steps.Functions;
        var all=Lua50.All(root).SelectMany(f=>f.Constants).OfType<byte[]>().Where(b=>b.All(c=>c<128)).Select(Encoding.ASCII.GetString).ToHashSet(StringComparer.Ordinal);
        var names=owned.Where(all.Contains).ToDictionary(n=>n,n=>"DS1_Havel_"+n,StringComparer.Ordinal);
        Lua50.RelocateIfCallbacks(root,names,new Dictionary<int,int>{{6580,HavelParameters.Think}},nativeCommon.Files.Single(f=>Leaf(f.Name)=="if.lua").Bytes.Span);
        var bytes=Lua50.Rewrite(Lua50.Write(root),names,enums,new Dictionary<double,double>{{1012799,leashRegion}});
        destination=MapNativeAi.Merge(destination,native("script/aicommon.luabnd.dcx"),7040,[10000]);
        var target=BND4.Read(destination);var infoFile=target.Files.Single(f=>Leaf(f.Name).EndsWith(".luainfo",StringComparison.Ordinal));var info=LUAINFO.Read(infoFile.Bytes);
        Need(!info.Goals.Any(g=>g.ID==HavelParameters.Think||g.Name==Name),"owned goal occupied");
        string leaf=HavelParameters.Think+"_battle.lua";Need(!target.Files.Any(f=>Leaf(f.Name)==leaf),"owned script occupied");
        int id=1000;while(target.Files.Any(f=>f.ID==id))id++;
        target.Files.Add(new BinderFile(entry.Flags,id,@"N:\SPRJ\data\INTERROOT_ps4\script\ai\out\"+leaf,bytes));
        info.Goals.Add(new(HavelParameters.Think,Name,true,false,null));infoFile.Bytes=info.Write();
        var namesFile=target.Files.Single(f=>Leaf(f.Name).EndsWith(".luagnl",StringComparison.Ordinal));var globals=LUAGNL.Read(namesFile.Bytes);var existing=globals.Globals.ToHashSet(StringComparer.Ordinal);
        foreach(string name in names.Values.Order(StringComparer.Ordinal))if(existing.Add(name))globals.Globals.Add(name);
        namesFile.Bytes=globals.Write();target.Files.Sort((a,b)=>a.ID.CompareTo(b.ID));return target.Write(DCX.Type.DCX_DFLT_10000_44_9);
    }
}
