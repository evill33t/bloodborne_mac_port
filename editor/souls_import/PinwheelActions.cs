// SPDX-License-Identifier: GPL-3.0-or-later
using SoulsFormats;
internal sealed class PinwheelActions {
    internal readonly SourceCharacterActions Source;
    internal readonly Func<string,byte[]?> Reader;
    internal PinwheelActions(Func<string,byte[]?> source) {
        const string path="chr/c3320.anibnd.dcx";var bank=BND3.Read(source(path)??throw new FileNotFoundException(path));
        var file=bank.Files.Single(f=>TAE.Is(f.Bytes.ToArray()));var tae=TAE.Read(file.Bytes);
        var extra=tae.Animations.Where(a=>a.ID>=10000).OrderBy(a=>a.ID).ToArray();
        if(!extra.Select(a=>a.ID).SequenceEqual(new long[]{12000,12001,12002}))throw new InvalidDataException("Pinwheel helper death family changed");
        foreach(var row in extra)row.ID=9200+row.ID-12000;
        file.Bytes=tae.Write();var bytes=bank.Write(DCX.Type.DCX_DFLT_10000_44_9);
        Reader=p=>p==path?bytes:source(p);Source=new(Reader,"c3320");
    }
    internal static int Slot(SourceCharacterActions source,int native) {
        if(native/1000000 is not 0 and not 1)throw new InvalidDataException("Pinwheel native animation bank");int id=native%1000000;
        if(native/1000000==1&&id is >=2000 and <=2002)return 9200+id-2000;
        // The source helper's custom damage family12000..2 all resolves to
        // its disappearance pose3009. BB's c2730 ordinary reaction slots are
        // 2004..2014 rather than source2000..2; route that private helper bank
        // to the authored helper reaction, without changing the boss bank.
        if(native/1000000==1&&id is >=2004 and <=2014)return 9200;
        if(id is >=2300 and <3000)return source.Contains(2010)?2010:0;
        if(source.Contains(id))return id;
        return id switch {>=3000 and <4000=>3000,>=2200 and <2250=>2200,>=2250 and <2300=>2250,>=2000 and <2200=>2000,_=>0};
    }
}
