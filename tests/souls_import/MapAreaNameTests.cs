// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

static class MapAreaNameTests {
    static void Check(bool value,string why){if(!value)throw new Exception(why);}
    static FMG Names(params (int Id,string Text)[] entries) {
        var fmg=new FMG(FMG.FMGVersion.DarkSouls3);
        foreach(var entry in entries)fmg.Entries.Add(new FMG.Entry(fmg,entry.Id,entry.Text));
        return fmg;
    }
    static BND4 Native(FMG names) {
        var binder=new BND4();
        binder.Files.Add(new BinderFile(Binder.FileFlags.Flag1,19,"locations.fmg",names.Write()));
        binder.Files.Add(new BinderFile(Binder.FileFlags.Flag1,10,"items.fmg",new byte[]{1,2,3,4}));
        return binder;
    }
    static byte[] Source(string name) {
        var binder=new BND3();
        binder.Files.Add(new BinderFile(Binder.FileFlags.Flag1,19,"locations.fmg",Names((1020,name),(2000,name)).Write()));
        return binder.Write();
    }
    internal static void Run() {
        var temporary=Directory.CreateTempSubdirectory("bbmap-area-name-test-");
        try {
            var sources=new Dictionary<string,byte[]> {
                ["msg/ENGLISH/item.msgbnd.dcx"]=Source("Firelink Shrine"),
                ["msg/GERMAN/item.msgbnd.dcx"]=Source("Feuerband-Schrein")
            };
            var originals=new Dictionary<string,byte[]>();
            foreach(var language in new[]{"engus","deude","turtr"}) {
                var path=Path.Combine(temporary.FullName,"msg",language,"item.msgbnd.dcx");
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                var native=Native(language=="deude"?Names((2410,"Zentral-Yharnam"),(30000,"Existing location")):Names((2410,"Central Yharnam")));
                originals[path]=native.Write(DCX.Type.DCX_DFLT_10000_44_9);File.WriteAllBytes(path,originals[path]);
            }
            var map=new MSBB();map.Models.Collisions.Add(new MSBB.Model.Collision{Name="h000000"});
            map.Parts.Collisions.Add(new MSBB.Part.Collision{Name="floor",ModelName="h000000",MapNameID=2410,PlayRegionID=2410000,EntityID=2413000});
            map.Parts.Collisions.Add(new MSBB.Part.Collision{Name="annex",ModelName="h000000",MapNameID=-2411,PlayRegionID=2410001});
            var outputs=new Dictionary<string,byte[]>();
            var result=MapAreaName.Export(map,"DS1R","m10_02_00_00",temporary.FullName,sources.GetValueOrDefault,outputs.Add);
            Check(result.MapNameId==30001&&result.EnglishName=="Firelink Shrine"&&result.CollisionCount==2,"Imported area name did not allocate an unused native short ID");
            Check(outputs.Count==3&&result.Languages.Count==3,"Not every installed language received the imported area name");
            Check(result.Languages.Single(n=>n.Language=="deude").Text=="Feuerband-Schrein"&&!result.Languages.Single(n=>n.Language=="deude").EnglishFallback,"Source localization was not preserved");
            Check(result.Languages.Single(n=>n.Language=="turtr").Text=="Firelink Shrine"&&result.Languages.Single(n=>n.Language=="turtr").EnglishFallback,"Missing source language must use the correct English area name");
            foreach(var (path,bytes) in outputs) {
                var before=BND4.Read(originals[Path.Combine(temporary.FullName,path)]);var after=BND4.Read(bytes);
                Check(before.Compression==after.Compression&&before.Files.Count==after.Files.Count,"Message binder metadata changed");
                Check(before.Files.Single(f=>f.ID==10).Bytes.Span.SequenceEqual(after.Files.Single(f=>f.ID==10).Bytes.Span),"Unrelated item-name FMG changed");
                var oldNames=FMG.Read(before.Files.Single(f=>f.ID==19).Bytes);var newNames=FMG.Read(after.Files.Single(f=>f.ID==19).Bytes);
                Check(newNames.Entries.Count==oldNames.Entries.Count+1&&oldNames.Entries.All(old=>newNames.Entries.Single(e=>e.ID==old.ID).Text==old.Text),"An existing location name was overwritten");
                Check(newNames.Entries.Single(e=>e.ID==result.MapNameId).Text==result.Languages.Single(n=>path.Contains("/"+n.Language+"/")).Text,"Exported name differs from its report");
            }
            Check(originals.All(pair=>File.ReadAllBytes(pair.Key).SequenceEqual(pair.Value)),"Native source message files were modified");
            var reopened=MSBB.Read(map.Write());
            Check(reopened.Parts.Collisions.All(p=>p.MapNameID==-30001)&&reopened.Parts.Collisions[0].PlayRegionID==2410000&&reopened.Parts.Collisions[0].EntityID==2413000,"Area-name collision references must request native banner refresh without losing gameplay metadata");
            foreach(var profile in new[]{"DS2","DS2S"}) {
                var majula=Names((10040000,"Majula"));var german=Names((10040000,"Majula"));
                byte[]? Read(string path)=>path switch {"menu/text/english/mapname.fmg"=>majula.Write(),"menu/text/germany/mapname.fmg"=>german.Write(),_=>null};
                var names=MapAreaName.Export(map,profile,"m10_04_00_00",temporary.FullName,Read,(_,_)=>{});
                Check(names.EnglishName=="Majula"&&!names.Languages.Single(n=>n.Language=="deude").EnglishFallback,"Majula names were not read from the source FMG");
                Check(names.MapNameId==30001&&map.Parts.Collisions.All(p=>p.MapNameID==-names.MapNameId),"Majula must keep positive FMG IDs and request the name through signed native collision IDs");
            }
            Console.WriteLine("PASS: imported area names use isolated collision IDs and source localizations; existing native text and source binders remain unchanged");
        } finally {temporary.Delete(true);}
    }
}
