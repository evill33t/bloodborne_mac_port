// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;

// A bounded merchant adapter: source greeting and stock, native purchase UI.
// Equipment, hostility, source quests and talk animations remain separate.
internal static class MapMerchantTalk {
    internal sealed record Stock(int SourceRow,int SourceGood,int NativeRow,int NativeGood,int Price,int Quantity,int StockFlag,string Conversion);
    internal sealed record Skipped(int SourceRow,int SourceGood,string Reason);
    internal sealed record Actor(string Name,int SourceEntity,int NativeEntity,int SourceTalk,int NativeTalk,int Greeting,int Lines,IReadOnlyList<Stock> Stock);
    internal sealed record Result(IReadOnlyList<Actor> Actors,IReadOnlyList<Stock> ShopRows,IReadOnlyList<Skipped> Skipped,IReadOnlyList<string> Limits);
    sealed record Plan(string Map,string Name,int Entity,int Talk,int Greeting,int GreetingState,int SourceShop,int Shop,int Text,int StockFlag);
    static readonly Plan[] Plans=[
        new("m10_02_00_00","Undead Female Merchant",6240,102624,34000000,27,1200,90020000,900080000,52100960),
        new("m10_01_00_00","Undead Male Merchant",6230,101623,33000000,29,1100,90021000,900081000,52100965)
    ];
    static string Leaf(string p)=>p.Replace('\\','/').Split('/').Last();
    static void Check(bool value,string why){if(!value)throw new InvalidDataException("Imported merchant: "+why);}
    static int Number(PARAM.Row row,string field)=>Convert.ToInt32(row[field].Value);
    static void Int(byte[] b,int at,int n)=>BitConverter.GetBytes(n).CopyTo(b,at);
    static PARAM Raw(Memory<byte> bytes,string type,int size,short version) {
        var p=PARAM.Read(bytes);Check(p.ParamType==type&&p.DetectedSize==size&&p.ParamdefDataVersion==version&&!p.BigEndian&&p.Format2D==PARAM.FormatFlags1.LongDataOffset,"native parameter schema changed: "+type);
        var d=new PARAMDEF{ParamType=type,DataVersion=version,Unicode=true};d.Fields.Add(new PARAMDEF.Field(d,PARAMDEF.DefType.dummy8,"payload"){ArrayLength=size});p.ApplyParamdef(d);return p;
    }
    static byte[] Payload(PARAM.Row row)=>((byte[])row.Cells.Single().Value).ToArray();
    static void Append(PARAM p,PARAM.Row donor,int id,string name,byte[] payload){Check(!p.Rows.Any(r=>r.ID==id),"owned parameter ID occupied: "+id);var row=new PARAM.Row(donor){ID=id,Name=name};row.Cells.Single().Value=payload;int at=p.Rows.FindIndex(r=>r.ID>id);if(at<0)p.Rows.Add(row);else p.Rows.Insert(at,row);}
    static byte[] Write(PARAM p){var nulls=p.Rows.Select((r,i)=>(r.Name,i)).Where(x=>x.Name==null).Select(x=>x.i).ToArray();var b=p.Write();foreach(int i in nulls)Array.Clear(b,64+i*24+16,8);return b;}
    static int Constant(byte[] b)=>b.Length==2&&b[0]<=127&&b[1]==0xa1?b[0]-64:b.Length==6&&b[0]==0x82&&b[5]==0xa1?BitConverter.ToInt32(b,1):throw new InvalidDataException("Merchant ESD constant changed");
    static string? Language(string value)=>value.ToLowerInvariant() switch {"engus" or "enggb"=>"ENGLISH","deude"=>"GERMAN","frafr"=>"FRENCH","itait"=>"ITALIAN","jpnjp" or "japanese"=>"JAPANESE","korkr"=>"KOREAN","polpl"=>"POLISH","rusru"=>"RUSSIAN","spaes"=>"SPANISH","spaar"=>"NSPANISH","porpt" or "porbr"=>"PORTUGUESE","zhocn"=>"SCHINESE","zhotw"=>"TCHINESE",_=>null};

    internal static Result Export(MSBB map,string sourceMap,string nativeMap,string nativeRoot,string definitions,
        Func<string,byte[]?> readSource,Func<string,byte[]?> readComposed,IReadOnlyDictionary<int,int> actorEntities,Action<string,byte[]> write,
        IEnumerable<KeyValuePair<string,byte[]>>? composedFiles=null) {
        var plan=Plans.SingleOrDefault(p=>p.Map==sourceMap)??throw new NotSupportedException("No bounded merchant for "+sourceMap);
        Check(nativeMap.Length==12&&nativeMap[0]=='m'&&int.TryParse(nativeMap.AsSpan(1,2),out _)&&int.TryParse(nativeMap.AsSpan(4,2),out int block)&&block<=9,"invalid target map");
        int area=int.Parse(nativeMap.AsSpan(1,2));block=int.Parse(nativeMap.AsSpan(4,2));
        Check(actorEntities.TryGetValue(plan.Entity,out int entity),"merchant actor mapping missing");var actor=map.Parts.Enemies.Single(a=>a.EntityID==entity);
        Check(actor.TalkID==-1&&actor.ModelName==(plan.Entity==6240?"c8903":"c8904"),"merchant actor ownership changed");
        byte[] Source(string path)=>readSource(path)??throw new FileNotFoundException("Source merchant resource unavailable",path);
        byte[] Native(string path)=>readComposed(path)??(File.Exists(Path.Combine(nativeRoot,path))?File.ReadAllBytes(Path.Combine(nativeRoot,path)):throw new FileNotFoundException("Native merchant resource unavailable",path));
        var pending=new Dictionary<string,byte[]>(StringComparer.Ordinal);var sourceGame=BND3.Read(Source("param/GameParam/GameParam.parambnd.dcx"));
        PARAM FromSource(string name){var p=PARAM.Read(sourceGame.Files.Single(f=>Leaf(f.Name)==name+".param").Bytes);p.ApplyParamdef(PARAMDEF.XmlDeserialize(Path.Combine(definitions,name+".xml")));return p;}
        var sourceGoods=FromSource("EquipParamGoods");var sourceShop=FromSource("ShopLineupParam");var sourceTalk=FromSource("TalkParam");
        Check(sourceShop.ParamType=="SHOP_LINEUP_PARAM"&&sourceShop.DetectedSize==32&&sourceShop.ParamdefDataVersion==1&&sourceTalk.DetectedSize==32,"source parameter schema changed");
        var sourceEsd=ESD.Read(BND3.Read(Source("script/talk/"+sourceMap+".talkesdbnd.dcx")).Files.Single(f=>Leaf(f.Name)==$"t{plan.Talk}.esd").Bytes);
        Check(sourceEsd.DarkSoulsCount==1&&sourceEsd.StateGroups.TryGetValue(1,out var sourceStates),"source merchant ESD format changed");sourceStates=sourceEsd.StateGroups[1];
        Check(sourceStates[plan.GreetingState].EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==1&&c.Arguments.Count==3&&Constant(c.Arguments[0])==plan.Greeting),"source greeting branch changed");
        Check(sourceStates.Values.Any(s=>s.EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==22&&c.Arguments.Count==2&&Constant(c.Arguments[0])==plan.SourceShop&&Constant(c.Arguments[1])==plan.SourceShop+99)),"source stock range changed");
        // Shipped BB t210691, group2147483646/state1, buys100000..109999.
        // Its menu-close test is CheckSpecificPersonMenuIsOpen(11,0)==0. Do not
        // import source UI numeric operations merely because they look alike.
        var donorTalk=BND4.Read(File.ReadAllBytes(Path.Combine(nativeRoot,"script/talk/m21_00_00_00.talkesdbnd.dcx")));
        var template=ESD.Read(donorTalk.Files.Single(f=>Leaf(f.Name)=="t210691.esd").Bytes);var nativeShop=template.StateGroups[2147483646][1];
        Check(template.LongFormat&&template.DarkSoulsCount==2&&nativeShop.EntryCommands.Any(c=>c.CommandBank==1&&c.CommandID==22&&c.Arguments.Count==2&&Constant(c.Arguments[0])==100000&&Constant(c.Arguments[1])==109999)&&nativeShop.Conditions.Any(c=>c.Evaluator.SequenceEqual(new byte[]{0x7b,0x4b,0x40,0x86,0xa6,0x40,0x95,0xa1})),"native purchase UI donor changed");
        var game=BND4.Read(Native("param/gameparam/gameparam.parambnd.dcx"));BinderFile Entry(string name)=>game.Files.Single(f=>Leaf(f.Name)==name+".param");
        var shops=Raw(Entry("ShopLineupParam").Bytes,"SHOP_LINEUP_PARAM",32,1);var goods=Raw(Entry("EquipParamGoods").Bytes,"EQUIP_PARAM_GOODS_ST",108,1);var talks=Raw(Entry("TalkParam").Bytes,"TALK_PARAM_ST",68,3);
        string bbDefs=Path.GetFullPath(Path.Combine(definitions,"../../BB/Defs"));var goodsDef=PARAMDEF.XmlDeserialize(Path.Combine(bbDefs,"EquipParamGoods.xml"));var goodsTyped=PARAM.Read(Entry("EquipParamGoods").Bytes);goodsTyped.ApplyParamdef(goodsDef);
        var nativeShopDef=PARAMDEF.XmlDeserialize(Path.Combine(bbDefs,"ShopLineupParam.xml"));Check(nativeShopDef.GetRowSize()==32&&nativeShopDef.Fields.Select(f=>f.InternalName).SequenceEqual(new[]{"equipId","value","mtrlId","eventFlag","qwcId","sellQuantity","shopType","equipType","value_SAN","pad_0"}),"native shop field order changed");
        var donor=shops.Rows.Single(r=>r.ID==100000);var donorBytes=Payload(donor);Check(BitConverter.ToInt32(donorBytes,0)==1000&&BitConverter.ToInt32(donorBytes,4)==180&&BitConverter.ToInt32(donorBytes,8)==-1&&BitConverter.ToInt32(donorBytes,12)==-1&&BitConverter.ToInt16(donorBytes,20)==-1&&donorBytes[22]==0&&donorBytes[23]==3&&BitConverter.ToInt16(donorBytes,24)==-1,"native goods/currency shop layout changed");
        var finite=Payload(shops.Rows.Single(r=>r.ID==100607));Check(BitConverter.ToInt32(finite,0)==4011&&BitConverter.ToInt16(finite,20)==1&&BitConverter.ToInt32(finite,12)==70009250,"native finite-stock contract changed");
        var usedFlags=new HashSet<int>();void Scan(ReadOnlySpan<byte> bytes){for(int i=0;i+4<=bytes.Length;i++){int n=BitConverter.ToInt32(bytes[i..]);if(n is >=52100960 and <52100970)usedFlags.Add(n);}}
        foreach(var f in game.Files)Scan(f.Bytes.Span);
        foreach(string folder in new[]{"event","script/talk"})foreach(string file in Directory.EnumerateFiles(Path.Combine(nativeRoot,folder),"*.dcx")){string path=Path.GetRelativePath(nativeRoot,file).Replace('\\','/');var bytes=Native(path);ReadOnlyMemory<byte> decoded=DCX.Is(bytes)?DCX.Decompress(bytes):bytes;Scan(decoded.Span);}
        foreach(var (path,bytes) in composedFiles??[])if(path.StartsWith("event/",StringComparison.Ordinal)||path.StartsWith("script/talk/",StringComparison.Ordinal)){ReadOnlyMemory<byte> decoded=DCX.Is(bytes)?DCX.Decompress(bytes):bytes;Scan(decoded.Span);}
        var stock=new List<Stock>();var skipped=new List<Skipped>();var keys=new Dictionary<int,int>();int limited=0;
        foreach(var original in sourceShop.Rows.Where(r=>r.ID>=plan.SourceShop&&r.ID<plan.SourceShop+100)) {
            int id=Number(original,"equipId");if(Number(original,"equipType")!=3){skipped.Add(new(original.ID,id,"Source equipment/ammunition is excluded."));continue;}
            var sourceGood=sourceGoods.Rows.Single(r=>r.ID==id);var conversion=MapItemPolicy.Map(MapItemPolicy.SourceGoodsCategory,id,1,MapItemPolicy.IsQuest(sourceGood));
            if(conversion==null){skipped.Add(new(original.ID,id,"No verified native goods conversion."));continue;}
            Check(Number(original,"shopType")==0&&Number(original,"mtrlId")==-1&&Number(original,"qwcId")==-1,"source shop requires unsupported currency/gates");
            int price=Number(original,"value"),quantity=Number(original,"sellQuantity");Check(price is >=0 and <=10000000&&quantity is -1 or 1,"source stock quantity/cost unsupported");
            int flag=-1;if(quantity==1){Check(limited<5,"merchant stock flag reservation exhausted");flag=plan.StockFlag+limited++;Check(!usedFlags.Contains(flag),"merchant stock flag occupied");usedFlags.Add(flag);}
            if(conversion.Kind=="quest-item") {
                string owner="Imported DS1R good "+id;var existing=goods.Rows.SingleOrDefault(r=>r.ID==conversion.TargetID);Check(existing==null||existing.Name==owner,"quest goods reservation occupied");
                var typed=new PARAM.Row(goodsTyped.Rows.Single(r=>r.ID==4000)){ID=conversion.TargetID,Name=owner};typed["maxNum"].Value=(short)Math.Clamp(Number(sourceGood,"maxNum"),1,99);typed["isConsume"].Value=(byte)0;
                goodsTyped.Rows.RemoveAll(r=>r.ID==conversion.TargetID);goodsTyped.Rows.Add(typed);var encoded=Raw(goodsTyped.Write(),"EQUIP_PARAM_GOODS_ST",108,1);var expected=Payload(encoded.Rows.Single(r=>r.ID==conversion.TargetID));
                Check(existing==null||Payload(existing).SequenceEqual(expected),"existing quest good differs from MapItems inventory identity");
                if(existing==null)Append(goods,goods.Rows.Single(r=>r.ID==4000),conversion.TargetID,owner,expected);keys[id]=conversion.TargetID;
            } else Check(goods.Rows.Any(r=>r.ID==conversion.TargetID),"native converted good is missing");
            int nativeID=plan.Shop+stock.Count;var data=donorBytes.ToArray();Int(data,0,conversion.TargetID);Int(data,4,price);Int(data,12,flag);BitConverter.GetBytes((short)quantity).CopyTo(data,20);Append(shops,donor,nativeID,"Imported merchant "+plan.Entity+" stock "+original.ID,data);stock.Add(new(original.ID,id,nativeID,conversion.TargetID,price,quantity,flag,conversion.Kind));
        }
        Check(stock.Count>0,"merchant has no supported stock");
        var menuLocales=new List<(string Language,BND4 Binder,BinderFile File,FMG Text)>();var sourceTexts=new Dictionary<string,BND3>(StringComparer.Ordinal);
        BND3? SourceText(string language,string kind){string key=language+"/"+kind;if(sourceTexts.TryGetValue(key,out var b))return b;var data=readSource($"msg/{language}/{kind}.msgbnd.dcx")??readSource($"msg/{language}/{kind}.msgbnd");if(data==null)return null;b=BND3.Read(data);sourceTexts.Add(key,b);return b;}
        var english=SourceText("ENGLISH","menu")??throw new FileNotFoundException("Source greeting text unavailable");var englishTalk=FMG.Read(english.Files.Single(f=>f.ID==1).Bytes).Entries.ToDictionary(e=>e.ID,e=>e.Text);
        foreach(string directory in Directory.EnumerateDirectories(Path.Combine(nativeRoot,"msg")).Order(StringComparer.Ordinal)) {
            string language=Path.GetFileName(directory),path=$"msg/{language}/menu.msgbnd.dcx";if(!File.Exists(Path.Combine(nativeRoot,path)))continue;var b=BND4.Read(Native(path));var f=b.Files.Single(f=>f.ID==1);menuLocales.Add((language,b,f,FMG.Read(f.Bytes)));
        }
        Check(menuLocales.Count>0,"native subtitle locales missing");
        int lines=0;var talkDef=PARAMDEF.XmlDeserialize(Path.Combine(bbDefs,"TalkParam.xml"));
        for(int i=0;i<99;i++) {
            var from=sourceTalk.Rows.SingleOrDefault(r=>r.ID==plan.Greeting+i);if(from==null)break;
            Check(Number(from,"returnPos")==0&&Number(from,"eventId")==-1&&Number(from,"reactionId")==-1,"source greeting continuation unsupported");int message=Number(from,"msgId"),id=plan.Text+i;
            Check(englishTalk.TryGetValue(message,out string? fallback)&&!string.IsNullOrWhiteSpace(fallback),"source greeting line missing");
            var row=new PARAM.Row(id,"Imported merchant "+plan.Entity+" greeting "+from.ID,talkDef);foreach(var c in row.Cells)c.Value=-1;row["msgId"].Value=id;row["voiceId"].Value=-1;row["returnPos"].Value=0;
            var typed=PARAM.Read(Entry("TalkParam").Bytes);typed.ApplyParamdef(talkDef);typed.Rows.Add(row);var encoded=Raw(typed.Write(),"TALK_PARAM_ST",68,3);Append(talks,talks.Rows.First(),id,row.Name!,Payload(encoded.Rows.Single(r=>r.ID==id)));
            foreach(var locale in menuLocales){Check(!locale.Text.Entries.Any(e=>e.ID==id),"subtitle reservation occupied");var fromLocale=Language(locale.Language) is {} lang?SourceText(lang,"menu"):null;string? text=fromLocale==null?null:FMG.Read(fromLocale.Files.Single(f=>f.ID==1).Bytes).Entries.SingleOrDefault(e=>e.ID==message)?.Text;locale.Text.Entries.Add(new(locale.Text,id,string.IsNullOrWhiteSpace(text)?fallback:text));}lines++;
        }
        Check(lines is >0 and <99,"unbounded greeting");
        if(keys.Count>0)foreach(var locale in menuLocales){string path=$"msg/{locale.Language}/item.msgbnd.dcx";var b=BND4.Read(Native(path));var fallback=SourceText("ENGLISH","item")??throw new FileNotFoundException("Source key text unavailable");var from=Language(locale.Language) is {} language?SourceText(language,"item")??fallback:fallback;foreach(int fileID in new[]{10,20,24}){var f=b.Files.Single(f=>f.ID==fileID);var text=FMG.Read(f.Bytes);var sourceText=FMG.Read(from.Files.Single(f=>f.ID==fileID).Bytes);var eng=FMG.Read(fallback.Files.Single(f=>f.ID==fileID).Bytes);foreach(var (sourceID,id) in keys){string value=sourceText.Entries.SingleOrDefault(e=>e.ID==sourceID)?.Text??eng.Entries.SingleOrDefault(e=>e.ID==sourceID)?.Text??"";Check(fileID!=10||!string.IsNullOrWhiteSpace(value),"source key name missing");var old=text.Entries.SingleOrDefault(e=>e.ID==id);Check(old==null||old.Text==value,"key text reservation occupied");if(old==null)text.Entries.Add(new(text,id,value));}f.Bytes=text.Write();}pending.Add(path,b.Write(b.Compression));}
        string talkPath="script/talk/"+nativeMap+".talkesdbnd.dcx";var talk=BND4.Read(Native(talkPath));int talkID=area*10000+block*1000+975;
        Check(!talk.Files.Any(f=>Leaf(f.Name)==$"t{talkID}.esd")&&!map.Parts.Enemies.Any(a=>a.TalkID==talkID),"merchant talk ID occupied");
        var esd=Build(template,talkID,plan.Text,plan.Shop,plan.Shop+stock.Count-1);var encodedEsd=esd.Write();var check=ESD.Read(encodedEsd);Check(check.Name==$"t{talkID}"&&check.StateGroups.Count==1,"merchant ESD roundtrip failed");
        var donorFile=talk.Files[0];string prefix=donorFile.Name[..(donorFile.Name.LastIndexOfAny(['\\','/'])+1)];talk.Files.Add(new(donorFile.Flags,talk.Files.Max(f=>f.ID)+1,prefix+$"t{talkID}.esd",encodedEsd));
        Entry("ShopLineupParam").Bytes=Write(shops);Entry("EquipParamGoods").Bytes=Write(goods);Entry("TalkParam").Bytes=Write(talks);pending.Add("param/gameparam/gameparam.parambnd.dcx",game.Write(game.Compression));pending.Add(talkPath,talk.Write(talk.Compression));
        foreach(var locale in menuLocales){locale.File.Bytes=locale.Text.Write();pending.Add($"msg/{locale.Language}/menu.msgbnd.dcx",locale.Binder.Write(locale.Binder.Compression));}
        foreach(var (path,bytes) in pending)write(path,bytes);actor.TalkID=talkID;
        var report=new Actor(plan.Name,plan.Entity,entity,plan.Talk,talkID,plan.Text,lines,stock);
        return new([report],stock,skipped,["Bounded authentic source greeting followed by the native Blood Echo purchase UI. Source prices and unlimited/one-item stock are retained for supported conversions; source souls economy is approximate.","Equipment/ammunition and goods without verified conversions are omitted. Converted quest goods keep independent inventory identity; source effects and quest-specific usage require their own adapters.","Source voices, talk gestures, hostility, repeat branches and full merchant quests are not ported. Subtitles use source localizations with English fallback. Native interaction/purchase/stock/save behavior requires gameplay validation."]);
    }
    static byte[] V(int n)=>n is >=-64 and <=63?[(byte)(n+64)]:[0x82,..BitConverter.GetBytes(n)];
    static byte[] E(byte[] b)=>[..b,0xa1];static byte[] F(int n,params byte[][] args)=>[..V(n),..args.SelectMany(a=>a),(byte)(0x84+args.Length),0xa6];static byte[] Eq(byte[] a,byte[] b)=>[..a,..b,0x95];
    static ESD.CommandCall C(int n,params int[] args)=>new(1,n,args.Select(n=>E(V(n))).ToArray());
    internal static ESD Build(ESD template,int talk,int greeting,int first,int last) {
        var esd=new ESD(true,2){Name=$"t{talk}",Unk70=template.Unk70,Unk74=template.Unk74,Unk78=template.Unk78,Unk7C=template.Unk7C};var s=new Dictionary<long,ESD.State>();esd.StateGroups.Add(1,s);for(int i=0;i<6;i++)s.Add(i,new());
        void Go(int a,int b,byte[] expr)=>s[a].Conditions.Add(new(b,E(expr)));
        Go(0,1,V(1));Go(1,2,F(56,V(6000)));s[2].EntryCommands.Add(C(1,greeting,-1,-1));
        byte[] interrupt=[..Eq(F(3),V(1)),..Eq(F(19),V(1)),0x99,..F(1),..V(5),0x94,0x99];Go(2,4,interrupt);Go(2,3,Eq(F(57,V(0)),V(1)));
        s[3].EntryCommands.Add(C(22,first,last));Go(3,4,interrupt);Go(3,4,Eq(F(59,V(11),V(0)),V(0)));
        foreach(var c in new[]{C(71),C(8,0),C(9),C(36)})s[4].EntryCommands.Add(c);Go(4,5,V(1));Go(5,1,Eq(F(56,V(6000)),V(0)));return esd;
    }
}
