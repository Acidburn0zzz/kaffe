/*
 * Java core library component.
 *
 * Copyright (c) 2002
 *      Dalibor Topic <robilad@yahoo.com>.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file.
 */

package kaffe.util.locale;

import java.util.ListResourceBundle;

public class Language_de extends ListResourceBundle {
    public Object [] [] getContents() {
	return contents;
    }

    private Object [] [] contents = {
	{ "en_CA", "Englisch" },
	{ "fr_CA", "Franz�sisch" },
	{ "zh_CN", "Chinesisch" },
	{ "zh", "Chinesisch" },
	{ "en", "Englisch" },
	{ "fr_FR", "Franz�sisch" },
	{ "fr", "Franz�sisch" },
	{ "de", "Deutsch" },
	{ "de_DE", "Deutsch" },
	{ "it", "Italienisch" },
	{ "it_IT", "Italienisch" },
	{ "ja_JP", "Japanisch" },
	{ "ja", "Japanisch" },
	{ "ko_KR", "Koreanisch" },
	{ "ko", "Koreanisch" },
	{ "zh_TW", "Chinesisch" },
	{ "en_GB", "Englisch" },
	{ "en_US", "Englisch" }
    };
}
