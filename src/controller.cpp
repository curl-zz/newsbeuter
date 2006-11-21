#include <view.h>
#include <controller.h>
#include <sstream>
#include <cstdlib>
#include <iostream>

#include <nxml.h>

#include <sys/types.h>
#include <pwd.h>

#include <config.h>

using namespace noos;

controller::controller() : v(0), rsscache(0), url_file("urls"), cache_file("cache.db") {
	std::ostringstream cfgfile;

	if (getenv("HOME")) {
		config_dir = ::getenv("HOME");
	} else {
		struct passwd * spw = ::getpwuid(::getuid());
		if (spw) {
			config_dir = spw->pw_dir;
		} else {
			std::cout << "Fatal error: couldn't determine home directory!" << std::endl;
			std::cout << "Please set the HOME environment variable or add a valid user for UID " << ::getuid() << "!" << std::endl;
			::exit(EXIT_FAILURE);
		}
	}

	config_dir.append(NOOS_PATH_SEP);
	config_dir.append(NOOS_CONFIG_SUBDIR);
	mkdir(config_dir.c_str(),0700); // create configuration directory if it doesn't exist

	url_file = config_dir + std::string(NOOS_PATH_SEP) + url_file;
	cache_file = config_dir + std::string(NOOS_PATH_SEP) + cache_file;
}

controller::~controller() {
	if (rsscache)
		delete rsscache;
}

void controller::set_view(view * vv) {
	v = vv;
}

void controller::run(int argc, char * argv[]) {
	int c;

	bool do_import = false, do_export = false;
	std::string importfile;

	do {
		c = ::getopt(argc,argv,"i:ehu:c:");
		if (c < 0)
			continue;
		switch (c) {
			case ':': /* fall-through */
			case '?': /* missing option */
				usage(argv[0]);
				break;
			case 'i':
				if (do_export)
					usage(argv[0]);
				do_import = true;
				importfile = optarg;
				break;
			case 'e':
				if (do_import)
					usage(argv[0]);
				do_export = true;
				break;
			case 'h':
				usage(argv[0]);
				break;
			case 'u':
				url_file = optarg;
				break;
			case 'c':
				cache_file = optarg;
				break;
			default:
				std::cout << argv[0] << ": unknown option: -" << static_cast<char>(c) << std::endl;
				usage(argv[0]);
				break;
		}
	} while (c != -1);

	cfg.load_config(url_file);

	if (do_import) {
		import_opml(importfile.c_str());
		return;
	}

	if (cfg.get_urls().size() == 0) {
		std::cout << "Error: no URLs configured. Please fill the file " << url_file << " with RSS feed URLs or import an OPML file." << std::endl << std::endl;
		usage(argv[0]);
	}

	if (!do_export) {
		std::cout << "Starting " << PROGRAM_NAME << " " << PROGRAM_VERSION << "..." << std::endl << std::endl;
	}

	if (!do_export)
		std::cout << "Loading articles from cache...";
	std::cout.flush();

	rsscache = new cache(cache_file.c_str());

	for (std::vector<std::string>::const_iterator it=cfg.get_urls().begin(); it != cfg.get_urls().end(); ++it) {
		rss_feed feed;
		feed.rssurl() = *it;
		rsscache->internalize_rssfeed(feed);
		feeds.push_back(feed);
	}
	rsscache->cleanup_cache(feeds);
	if (!do_export)
		std::cout << "done." << std::endl;

	if (do_export) {
		export_opml();
		return;
	}

	v->set_feedlist(feeds);
	v->run_feedlist();
}

void controller::open_item(rss_item& item) {
	v->run_itemview(item);
	item.unread() = false; // XXX: see TODO list
}

void controller::open_feed(unsigned int pos) {
	if (pos < feeds.size()) {
		v->feedlist_status("Opening feed...");

		rss_feed& feed = feeds[pos];

		v->feedlist_status("");

		if (feed.items().size() == 0) {
			v->feedlist_error("Error: feed contains no items!");
		} else {
			v->run_itemlist(feed);
			rsscache->externalize_rssfeed(feed); // save possibly changed unread flags
			v->set_feedlist(feeds);
		}
	} else {
		v->feedlist_error("Error: invalid feed!");
	}
}

void controller::reload(unsigned int pos) {
	if (pos < feeds.size()) {
		rss_feed feed = feeds[pos];
		std::string msg = "Loading ";
		msg.append(feed.rssurl());
		msg.append("...");
		v->feedlist_status(msg.c_str());
		rss_parser parser(feed.rssurl().c_str());
		feed = parser.parse();
		rsscache->externalize_rssfeed(feed);
		rsscache->internalize_rssfeed(feed);
		feeds[pos] = feed;
		v->feedlist_status("");
		v->set_feedlist(feeds);
	} else {
		v->feedlist_error("Error: invalid feed!");
	}
}

void controller::reload_all() {
	for (unsigned int i=0;i<feeds.size();++i) {
		this->reload(i);
	}
}

void controller::usage(char * argv0) {
	std::cout << PROGRAM_NAME << " " << PROGRAM_VERSION << std::endl;
	std::cout << "usage: " << argv0 << " [-i <file>|-e] [-u <urlfile>] [-c <cachefile>] [-h]" << std::endl;
	std::cout << "-e              export OPML feed to stdout" << std::endl;
	std::cout << "-i <file>       import OPML file" << std::endl;
	std::cout << "-u <urlfile>    read RSS feed URLs from <urlfile>" << std::endl;
	std::cout << "-c <cachefile>  use <cachefile> as cache file" << std::endl;
	std::cout << "-h              this help" << std::endl;
	::exit(EXIT_FAILURE);
}

void controller::import_opml(const char * filename) {
	nxml_t *data;
	nxml_data_t * root, * body, * outline;
	nxml_error_t ret;

	ret = nxml_new (&data);
	if (ret != NXML_OK) {
		puts (nxml_strerror (ret)); // TODO
		return;
	}

	ret = nxml_parse_file (data, const_cast<char *>(filename));
	if (ret != NXML_OK) {
		puts (nxml_strerror (ret)); // TODO
		return;
	}

	nxml_root_element (data, &root);

	if (root) {
		body = nxmle_find_element(data, root, "body", NULL);
		if (body) {
			rec_find_rss_outlines(body);
			cfg.write_config();
		}
	}

	nxml_free(data);
	std::cout << "Import of " << filename << " finished." << std::endl;
}

void controller::export_opml() {
	std::cout << "<?xml version=\"1.0\"?>" << std::endl;
	std::cout << "<opml version=\"1.0\">" << std::endl;
	std::cout << "\t<head>" << std::endl << "\t\t<title>noos - Exported Feeds</title>" << std::endl << "\t</head>" << std::endl;
	std::cout << "\t<body>" << std::endl;
	for (std::vector<rss_feed>::iterator it=feeds.begin(); it != feeds.end(); ++it) {
		std::cout << "\t\t<outline type=\"rss\" xmlUrl=\"" << it->rssurl() << "\" title=\"" << it->title() << "\" />" << std::endl;
	}
	std::cout << "\t</body>" << std::endl;
	std::cout << "</opml>" << std::endl;
}

void controller::rec_find_rss_outlines(nxml_data_t * node) {
	while (node) {
		char * url = nxmle_find_attribute(node, "xmlUrl", NULL);
		char * type = nxmle_find_attribute(node, "type", NULL);

		if (node->type == NXML_TYPE_ELEMENT && strcmp(node->value,"outline")==0 && type && strcmp(type,"rss")==0 && url) {

			bool found = false;

			for (std::vector<std::string>::iterator it = cfg.get_urls().begin(); it != cfg.get_urls().end(); ++it) {
				if (*it == url) {
					found = true;
				}
			}

			if (!found) {
				cfg.get_urls().push_back(std::string(url));
			}

		}

		rec_find_rss_outlines(node->children);

		node = node->next;
	}
}