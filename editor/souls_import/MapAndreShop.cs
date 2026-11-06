// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// One real quest good, source price and finite stock, native Blood Echo shop.
// This does not enable smithing, repair, reinforcement or Ember services.
internal static class MapAndreShop {
    internal const int Shop=90022000,Good=902002;
    internal static readonly int[] StockCounterFlags=Enumerable.Range(52411400,32).ToArray();
    internal sealed record Result(int SourceRow,int SourceGood,int NativeRow,int NativeGood,int Price,int Quantity,int SourceStockFlag,int NativeStockFlag,int Locales);
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Need(bool b,string why){if(!b)throw new InvalidDataException("Andre Crest shop: "+why);}
    static int N(PARAM.Row row,string field)=>Convert.ToInt32(row[field].Value);
    static void Int(byte[] b,int at,int n)=>BitConverter.GetBytes(n).CopyTo(b,at);
    static int Constant(byte[] b)=>b.Length==2?b[0]-64:BitConverter.ToInt32(b,1);
    static string? Language(string value)=>value.ToLowerInvariant() switch {"engus" or "enggb"=>"ENGLISH","deude"=>"GERMAN","frafr"=>"FRENCH","itait"=>"ITALIAN","jpnjp" or "japanese"=>"JAPANESE","korkr"=>"KOREAN","polpl"=>"POLISH","rusru"=>"RUSSIAN","spaes"=>"SPANISH","spaar"=>"NSPANISH","porpt" or "porbr"=>"PORTUGUESE","zhocn"=>"SCHINESE","zhotw"=>"TCHINESE",_=>null};
    internal static Result Apply(Func<string,byte[]?> source,Func<string,byte[]> native,Func<string,byte[]?> composed,string definitions,string nativeRoot,Dictionary<string,byte[]> files) {
        byte[] ReadSource(string path)=>source(path)??throw new FileNotFoundException("Andre source stock dependency unavailable",path);
        byte[] Read(string path)=>files.TryGetValue(path,out var bytes)?bytes:composed(path)??native(path);
        var sourceGame=BND3.Read(ReadSource("param/GameParam/GameParam.parambnd.dcx"));
        PARAM SourceParam(string name){var p=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,name+".xml")));return p;}
        var stock=SourceParam("ShopLineupParam").Rows.Single(r=>r.ID==1401);var crest=SourceParam("EquipParamGoods").Rows.Single(r=>r.ID==2002);
        Need(new[]{N(stock,"equipId"),N(stock,"value"),N(stock,"mtrlId"),N(stock,"eventFlag"),N(stock,"qwcId"),N(stock,"sellQuantity"),N(stock,"shopType"),N(stock,"equipType")}.SequenceEqual(new[]{2002,20000,-1,11017140,-1,1,0,3}),"source Crest price, currency, identity or stock changed");
        Need(MapItemPolicy.IsQuest(crest)&&N(crest,"maxNum")==1&&MapItemPolicy.QuestID(2002)==Good,"source Crest quest-good identity changed");
        var donorTalk=BND4.Read(native("script/talk/m21_00_00_00.talkesdbnd.dcx"));var donor=ESD.Read(donorTalk.Files.Single(f=>Leaf(f.Name)=="t210691.esd").Bytes);var nativeShop=donor.StateGroups[2147483646][1];
        Need(nativeShop.EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==22&&c.Arguments.Select(Constant).SequenceEqual(new[]{100000,109999}))&&nativeShop.Conditions.Any(c=>c.Evaluator.SequenceEqual(new byte[]{0x7b,0x4b,0x40,0x86,0xa6,0x40,0x95,0xa1})),"native purchase UI precedent changed");
        var game=BND4.Read(Read("param/gameparam/gameparam.parambnd.dcx"));BinderFile Entry(string name)=>game.Files.Single(f=>Leaf(f.Name)==name+".param");
        var shops=ImportParam.Read(Entry("ShopLineupParam").Bytes,"SHOP_LINEUP_PARAM",32,1);var goods=ImportParam.Read(Entry("EquipParamGoods").Bytes,"EQUIP_PARAM_GOODS_ST",108,1);
        Need(shops.Rows.All(r=>r.ID!=Shop),"owned shop row occupied");
        var shopDonor=shops.Rows.Single(r=>r.ID==100000);var data=ImportParam.Payload(shopDonor);
        Need(BitConverter.ToInt32(data)==1000&&BitConverter.ToInt32(data,4)==180&&BitConverter.ToInt32(data,8)==-1&&BitConverter.ToInt32(data,12)==-1&&BitConverter.ToInt16(data,20)==-1&&data[22]==0&&data[23]==3&&BitConverter.ToInt16(data,24)==-1,"native Blood Echo goods shop ABI changed");
        var finite=ImportParam.Payload(shops.Rows.Single(r=>r.ID==100607));Need(BitConverter.ToInt16(finite,20)==1&&BitConverter.ToInt32(finite,12)==70009250,"native finite-stock precedent changed");
        int flag=MapNpcQuestState.Flag(11017140);Need(flag==StockCounterFlags[0]&&MapNpcQuestState.Flags.Values.Where(f=>f!=flag).All(f=>!StockCounterFlags.Contains(f)),"stock counter overlaps quest flags");
        MapQuestState.VerifyReservations(nativeRoot,StockCounterFlags);
        Need(shops.Rows.All(r=>{int other=BitConverter.ToInt32(ImportParam.Payload(r),12);return other<0||(long)other+31<flag||other>StockCounterFlags[^1];}),"owned stock counter range occupied");
        string bbDefs=Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs"));var goodsDef=PARAMDEF.XmlDeserialize(Path.Combine(bbDefs,"EquipParamGoods.xml"));var typed=PARAM.Read(Entry("EquipParamGoods").Bytes);typed.ApplyParamdef(goodsDef);
        var key=new PARAM.Row(typed.Rows.Single(r=>r.ID==4000)){ID=Good,Name="Imported DS1R good 2002"};key["maxNum"].Value=(short)1;key["isConsume"].Value=(byte)0;typed.Rows.RemoveAll(r=>r.ID==Good);typed.Rows.Add(key);
        var encoded=ImportParam.Read(typed.Write(),"EQUIP_PARAM_GOODS_ST",108,1);var expected=ImportParam.Payload(encoded.Rows.Single(r=>r.ID==Good));var existing=goods.Rows.SingleOrDefault(r=>r.ID==Good);
        Need(existing==null||existing.Name==key.Name&&ImportParam.Payload(existing).SequenceEqual(expected),"existing Crest good conflicts with shared item policy");
        if(existing==null)ImportParam.Append(goods,goods.Rows.Single(r=>r.ID==4000),Good,key.Name!,expected);
        Int(data,0,Good);Int(data,4,20000);Int(data,12,flag);BitConverter.GetBytes((short)1).CopyTo(data,20);ImportParam.Append(shops,shopDonor,Shop,"Imported Andre Crest of Artorias1401",data);
        var texts=new Dictionary<string,BND3>(StringComparer.Ordinal);
        BND3? Text(string language){if(texts.TryGetValue(language,out var old))return old;var bytes=source("msg/"+language+"/item.msgbnd.dcx")??source("msg/"+language+"/item.msgbnd");if(bytes==null)return null;var bank=BND3.Read(bytes);texts.Add(language,bank);return bank;}
        var english=Text("ENGLISH")??throw new InvalidDataException("Source Crest text unavailable");int locales=0;
        foreach(string dir in Directory.EnumerateDirectories(Path.Combine(nativeRoot,"msg")).Order(StringComparer.Ordinal)) {
            string language=Path.GetFileName(dir),path="msg/"+language+"/item.msgbnd.dcx";if(!File.Exists(Path.Combine(nativeRoot,path)))continue;
            var bank=BND4.Read(Read(path));var sourceText=Language(language) is {} name?Text(name)??english:english;
            foreach(int id in new[]{10,20,24}) {
                var f=bank.Files.Single(f=>f.ID==id);var target=FMG.Read(f.Bytes);var local=FMG.Read(sourceText.Files.Single(f=>f.ID==id).Bytes);var fallback=FMG.Read(english.Files.Single(f=>f.ID==id).Bytes);
                string value=local.Entries.SingleOrDefault(e=>e.ID==2002)?.Text??fallback.Entries.SingleOrDefault(e=>e.ID==2002)?.Text??"";Need(id!=10||!string.IsNullOrWhiteSpace(value),"source Crest name missing");
                var old=target.Entries.SingleOrDefault(e=>e.ID==Good);Need(old==null||old.Text==value,"existing Crest localization conflicts with source item");if(old==null)target.Entries.Add(new(target,Good,value));f.Bytes=target.Write();
            }
            files[path]=bank.Write(bank.Compression);locales++;
        }
        Need(locales>0,"native item locales unavailable");Entry("ShopLineupParam").Bytes=ImportParam.Write(shops);Entry("EquipParamGoods").Bytes=ImportParam.Write(goods);files["param/gameparam/gameparam.parambnd.dcx"]=game.Write(game.Compression);
        return new(1401,2002,Shop,Good,20000,1,11017140,flag,locales);
    }
}
