#!/usr/bin/env python3
"""Adds the hidden-network and DNS strings to every language file.

The interface is built from tags (see lang.h): a tag with no line in a
language's .ini is shown as the tag itself, so a string exists in all twelve
files or it is a word like "wifi_dns_saved" on screen. This puts the nine new
ones after `wifi_available_networks` in each file, with the text the language
already uses for the surrounding lines.

    python3 tools/lang_wifi_net.py            # writes the files
    python3 tools/lang_wifi_net.py --check    # says what is missing, changes nothing
"""

import argparse
import pathlib
import sys

LANG_DIR = pathlib.Path("sonix-packer/assets/BLAZE/usr/resource/sonix/language")
ANCHOR = "wifi_available_networks"

# tag -> text per language file (without .ini). English is the fallback for a
# language that is missing from this table.
STRINGS = {
    "wifi_hidden_network": {
        "English": "Hidden network",
        "Čeština": "Skrytá síť",
        "Chinese": "隐藏网络",
        "Deutsch": "Verborgenes Netzwerk",
        "Español": "Red oculta",
        "Français": "Réseau masqué",
        "Italiano": "Rete nascosta",
        "Japanese": "隠れたネットワーク",
        "Nederlands": "Verborgen netwerk",
        "Polski": "Sieć ukryta",
        "Português BR": "Rede oculta",
        "Russian": "Скрытая сеть",
    },
    "wifi_ssid_invalid": {
        "English": "A network name is 1 to 32 characters",
        "Čeština": "Název sítě má 1 až 32 znaků",
        "Chinese": "网络名称长度为 1 到 32 个字符",
        "Deutsch": "Ein Netzwerkname hat 1 bis 32 Zeichen",
        "Español": "El nombre de la red tiene de 1 a 32 caracteres",
        "Français": "Un nom de réseau fait 1 à 32 caractères",
        "Italiano": "Il nome di una rete va da 1 a 32 caratteri",
        "Japanese": "ネットワーク名は 1〜32 文字です",
        "Nederlands": "Een netwerknaam is 1 tot 32 tekens",
        "Polski": "Nazwa sieci ma od 1 do 32 znaków",
        "Português BR": "O nome da rede tem de 1 a 32 caracteres",
        "Russian": "Имя сети — от 1 до 32 символов",
    },
    "wifi_dns_section": {
        "English": "DNS",
        "Čeština": "DNS",
        "Chinese": "DNS",
        "Deutsch": "DNS",
        "Español": "DNS",
        "Français": "DNS",
        "Italiano": "DNS",
        "Japanese": "DNS",
        "Nederlands": "DNS",
        "Polski": "DNS",
        "Português BR": "DNS",
        "Russian": "DNS",
    },
    "wifi_dns_primary": {
        "English": "Primary DNS",
        "Čeština": "Primární DNS",
        "Chinese": "首选 DNS",
        "Deutsch": "Primärer DNS",
        "Español": "DNS primario",
        "Français": "DNS principal",
        "Italiano": "DNS primario",
        "Japanese": "プライマリ DNS",
        "Nederlands": "Primaire DNS",
        "Polski": "Podstawowy DNS",
        "Português BR": "DNS primário",
        "Russian": "Основной DNS",
    },
    "wifi_dns_secondary": {
        "English": "Secondary DNS",
        "Čeština": "Sekundární DNS",
        "Chinese": "备用 DNS",
        "Deutsch": "Sekundärer DNS",
        "Español": "DNS secundario",
        "Français": "DNS secondaire",
        "Italiano": "DNS secondario",
        "Japanese": "セカンダリ DNS",
        "Nederlands": "Secundaire DNS",
        "Polski": "Zapasowy DNS",
        "Português BR": "DNS secundário",
        "Russian": "Дополнительный DNS",
    },
    "wifi_dns_not_set": {
        "English": "Not set",
        "Čeština": "Nenastaveno",
        "Chinese": "未设置",
        "Deutsch": "Nicht gesetzt",
        "Español": "Sin definir",
        "Français": "Non défini",
        "Italiano": "Non impostato",
        "Japanese": "未設定",
        "Nederlands": "Niet ingesteld",
        "Polski": "Nie ustawiono",
        "Português BR": "Não definido",
        "Russian": "Не задан",
    },
    "wifi_dns_invalid": {
        "English": "Not an IP address",
        "Čeština": "Není to IP adresa",
        "Chinese": "不是 IP 地址",
        "Deutsch": "Keine IP-Adresse",
        "Español": "No es una dirección IP",
        "Français": "Ce n'est pas une adresse IP",
        "Italiano": "Non è un indirizzo IP",
        "Japanese": "IP アドレスではありません",
        "Nederlands": "Geen IP-adres",
        "Polski": "To nie jest adres IP",
        "Português BR": "Não é um endereço IP",
        "Russian": "Это не IP-адрес",
    },
    "wifi_dns_saved": {
        "English": "DNS saved",
        "Čeština": "DNS uloženo",
        "Chinese": "DNS 已保存",
        "Deutsch": "DNS gespeichert",
        "Español": "DNS guardado",
        "Français": "DNS enregistré",
        "Italiano": "DNS salvato",
        "Japanese": "DNS を保存しました",
        "Nederlands": "DNS opgeslagen",
        "Polski": "Zapisano DNS",
        "Português BR": "DNS guardado",
        "Russian": "DNS сохранён",
    },
    "wifi_dns_reset": {
        "English": "Reset to the router's",
        "Čeština": "Vrátit na adresy routeru",
        "Chinese": "恢复为路由器 DNS",
        "Deutsch": "Auf die des Routers zurücksetzen",
        "Español": "Volver a las del router",
        "Français": "Revenir à celles du routeur",
        "Italiano": "Torna a quelle del router",
        "Japanese": "ルーターの DNS に戻す",
        "Nederlands": "Terug naar die van de router",
        "Polski": "Przywróć adresy routera",
        "Português BR": "Voltar às do router",
        "Russian": "Сбросить на адреса роутера",
    },
    "wifi_dns_write_failed": {
        "English": "Could not write the DNS settings",
        "Čeština": "Nastavení DNS se nepodařilo zapsat",
        "Chinese": "无法写入 DNS 设置",
        "Deutsch": "DNS-Einstellungen konnten nicht geschrieben werden",
        "Español": "No se pudo escribir la configuración de DNS",
        "Français": "Impossible d'écrire les réglages DNS",
        "Italiano": "Non è stato possibile scrivere le impostazioni DNS",
        "Japanese": "DNS 設定を書き込めませんでした",
        "Nederlands": "DNS-instellingen konden niet worden geschreven",
        "Polski": "Nie udało się zapisać ustawień DNS",
        "Português BR": "Não foi possível gravar as definições de DNS",
        "Russian": "Не удалось записать настройки DNS",
    },
}

TAGS = list(STRINGS)


def text(tag, language):
    table = STRINGS[tag]
    return table.get(language, table["English"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="report what is missing, write nothing")
    args = parser.parse_args()

    files = sorted(LANG_DIR.glob("*.ini"))
    if not files:
        sys.exit(f"no language files under {LANG_DIR}")

    missing_total = 0
    for path in files:
        language = path.stem
        lines = path.read_text(encoding="utf-8").splitlines(keepends=True)

        present = {tag for tag in TAGS if any(line.startswith(tag + " =") for line in lines)}
        missing = [tag for tag in TAGS if tag not in present]
        if not missing:
            print(f"{path.name}: already there")
            continue
        missing_total += len(missing)

        if args.check:
            print(f"{path.name}: missing {', '.join(missing)}")
            continue

        anchor = next((i for i, line in enumerate(lines) if line.startswith(ANCHOR + " =")), None)
        if anchor is None:
            sys.exit(f"{path.name}: no '{ANCHOR}' line to insert after")
        block = [f"{tag} = {text(tag, language)}\n" for tag in missing]
        lines[anchor + 1:anchor + 1] = block
        path.write_text("".join(lines), encoding="utf-8")
        print(f"{path.name}: added {len(block)}")

    if args.check and missing_total:
        sys.exit(1)


if __name__ == "__main__":
    main()
