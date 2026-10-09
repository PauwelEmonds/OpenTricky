// PackIso - a copy of the APK with the disc image in it as assets/game.iso, stored (not compressed), so the game reads it
// straight out of the installed APK (OpenTrickyActivity.openDiscImage). The old signature is left out: pack_iso.bat
// aligns and signs the result again.
//
//   java PackIso.java <in.apk> <image.iso> <out.apk>
import java.io.*;
import java.util.Enumeration;
import java.util.zip.*;

public class PackIso {
    static final String NAME = "assets/game.iso";

    public static void main(String[] args) throws Exception {
        if (args.length != 3) { System.err.println("usage: java PackIso.java <in.apk> <image.iso> <out.apk>"); System.exit(2); }
        File apk = new File(args[0]), iso = new File(args[1]), out = new File(args[2]);
        byte[] buf = new byte[1 << 20];
        // Android reads no ZIP64: the whole APK has to stay under 4 GB.
        if (apk.length() + iso.length() > 0xFFFFFFFFL - (64L << 20)) {
            System.err.println("The disc image is too big to go into an APK (the limit is 4 GB with the game).\n"
                + "A full 7-8 GB dump has to be trimmed first (extract-xiso -r makes the image just the game);\n"
                + "or install the APK without it and choose the image on the phone.");
            System.exit(3);
        }

        System.out.println("Checking the disc image (" + iso.length() / (1024 * 1024) + " MB)...");
        CRC32 crc = new CRC32();
        try (InputStream in = new FileInputStream(iso)) {
            for (int n; (n = in.read(buf)) > 0; ) crc.update(buf, 0, n);
        }

        System.out.println("Packing it into the APK...");
        try (ZipFile z = new ZipFile(apk);
             ZipOutputStream zo = new ZipOutputStream(new BufferedOutputStream(new FileOutputStream(out), 1 << 20))) {
            for (Enumeration<? extends ZipEntry> e = z.entries(); e.hasMoreElements(); ) {
                ZipEntry x = e.nextElement();
                String n = x.getName();
                if (n.equals(NAME)) continue;                                   // packed before: replaced
                if (n.startsWith("META-INF/") && (n.endsWith(".SF") || n.endsWith(".RSA") || n.endsWith(".EC")
                        || n.endsWith(".DSA") || n.equals("META-INF/MANIFEST.MF"))) continue;   // the old signature
                ZipEntry y = new ZipEntry(n);
                y.setTime(x.getTime());
                y.setMethod(x.getMethod());
                if (x.getMethod() == ZipEntry.STORED) {                         // resources.arsc, ...: kept stored
                    y.setSize(x.getSize());
                    y.setCompressedSize(x.getSize());
                    y.setCrc(x.getCrc());
                }
                zo.putNextEntry(y);
                try (InputStream in = z.getInputStream(x)) { in.transferTo(zo); }
                zo.closeEntry();
            }
            ZipEntry g = new ZipEntry(NAME);
            g.setMethod(ZipEntry.STORED);
            g.setSize(iso.length());
            g.setCompressedSize(iso.length());
            g.setCrc(crc.getValue());
            zo.putNextEntry(g);
            try (InputStream in = new FileInputStream(iso)) {
                for (int n; (n = in.read(buf)) > 0; ) zo.write(buf, 0, n);
            }
            zo.closeEntry();
        }
    }
}
