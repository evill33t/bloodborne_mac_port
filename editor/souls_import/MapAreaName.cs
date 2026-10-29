// SPDX-License-Identifier: GPL-2.0-or-later
using SoulsFormats;

internal static class MapAreaName {
    internal sealed record LocalizedName(string Language,string Text,bool EnglishFallback);
    internal sealed record Result(short MapNameId,string EnglishName,int CollisionCount,IReadOnlyList<LocalizedName> Languages);

    // The imported level receives a new name entry. Keeping native name IDs intact
    // also preserves lantern menus and other maps that still refer to Yharnam.
    internal static Result Export(MSBB map,string profile,string sourceMap,string nativeRoot,
        Func<string,byte[]?> readSource,Action<string,byte[]> write,Func<string,byte[]?>? readNative=null) {
        var sourceId=(profile,sourceMap) switch {
            ("DS1" or "DS1R","m10_02_00_00")=>1020,
            ("DS1" or "DS1R","m10_01_00_00")=>1010,
            ("DS2" or "DS2S","m10_04_00_00")=>10040000,
            _=>throw new NotSupportedException("No imported area-name mapping for "+profile+" "+sourceMap)
        };
        var cache=new Dictionary<string,string?>(StringComparer.Ordinal);
        string? ReadName(string language) {
            if(cache.TryGetValue(language,out var cached))return cached;
            var path=profile is "DS1" or "DS1R"?$"msg/{language}/item.msgbnd.dcx":$"menu/text/{language}/mapname.fmg";
            var bytes=readSource(path);
            if(bytes==null&&profile is "DS1" or "DS1R")bytes=readSource(path[..^4]);
            string? name=null;
            if(bytes!=null) {
                var fmg=profile is "DS1" or "DS1R"
                    ?FMG.Read(BND3.Read(bytes).Files.Single(f=>f.ID==19).Bytes)
                    :FMG.Read(bytes);
                name=fmg.Entries.SingleOrDefault(e=>e.ID==sourceId)?.Text;
                if(string.IsNullOrWhiteSpace(name))name=null;
            }
            cache.Add(language,name);return name;
        }
        var englishLanguage=profile is "DS1" or "DS1R"?"ENGLISH":"english";
        var english=ReadName(englishLanguage)??throw new InvalidDataException("Source English area name is unavailable");
        var folder=Path.Combine(nativeRoot,"msg");
        if(!Directory.Exists(folder))throw new DirectoryNotFoundException("Native message folder is unavailable: "+folder);
        var native=new List<(string Language,BND4 Binder,BinderFile File,FMG Names)>();
        foreach(var directory in Directory.EnumerateDirectories(folder).Order(StringComparer.Ordinal)) {
            var path=Path.Combine(directory,"item.msgbnd.dcx");
            if(!File.Exists(path))continue;
            var relative=$"msg/{Path.GetFileName(directory)}/item.msgbnd.dcx";
            var binder=BND4.Read(readNative?.Invoke(relative)??File.ReadAllBytes(path));var file=binder.Files.Single(f=>f.ID==19);
            native.Add((Path.GetFileName(directory),binder,file,FMG.Read(file.Bytes)));
        }
        if(native.Count==0)throw new InvalidDataException("No native location-name message binders found");
        if(map.Parts.Collisions.Count==0)throw new InvalidDataException("Imported area has no collision name references");
        var used=native.SelectMany(n=>n.Names.Entries).Select(e=>e.ID).ToHashSet();
        var id=Enumerable.Range(30000,short.MaxValue-30000+1).FirstOrDefault(candidate=>!used.Contains(candidate),-1);
        if(id<0)throw new InvalidDataException("No unused imported area-name ID fits the native collision field");
        var names=new List<LocalizedName>();
        foreach(var (language,binder,file,fmg) in native) {
            var sourceLanguage=SourceLanguage(language,profile);
            var localized=sourceLanguage==null?null:ReadName(sourceLanguage);
            var text=localized??english;
            fmg.Entries.Add(new FMG.Entry(fmg,id,text));
            file.Bytes=fmg.Write();
            write($"msg/{language}/item.msgbnd.dcx",binder.Write(binder.Compression));
            names.Add(new LocalizedName(language,text,localized==null));
        }
        // Native collision handling accepts a positive ID only before a name was
        // established. A negative ID requests abs(ID) and refreshes the area banner
        // when the name changes, including when loading an existing native save.
        foreach(var collision in map.Parts.Collisions)collision.MapNameID=checked((short)-id);
        return new Result((short)id,english,map.Parts.Collisions.Count,names);
    }

    static string? SourceLanguage(string native,string profile) {
        var language=native.ToLowerInvariant() switch {
            "engus" or "enggb"=>"ENGLISH",
            "deude"=>"GERMAN",
            "frafr"=>"FRENCH",
            "itait"=>"ITALIAN",
            "jpnjp" or "japanese"=>"JAPANESE",
            "korkr"=>"KOREAN",
            "polpl"=>"POLISH",
            "rusru"=>"RUSSIAN",
            "spaes"=>"SPANISH",
            "spaar"=>"NSPANISH",
            "porpt" or "porbr"=>"PORTUGUESE",
            "zhocn"=>"SCHINESE",
            "zhotw"=>"TCHINESE",
            _=>null
        };
        if(profile is "DS1" or "DS1R"||language==null)return language;
        return language switch {
            "GERMAN"=>"germany",
            "NSPANISH"=>"neutralspanish",
            "TCHINESE"=>"chinese",
            // Scholar's traditional and simplified Chinese fonts/strings differ.
            // Fall back to English when the installed source lacks the locale.
            "SCHINESE"=>null,
            _=>language.ToLowerInvariant()
        };
    }
}
