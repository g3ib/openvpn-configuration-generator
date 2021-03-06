// Copyright SparkLabs Pty Ltd 2018

#include "stdafx.h"
#include "Interactive.h"

Interactive::Interactive(String ^ path, OpenSSLHelper::Algorithm algorithm, int keySize, String^ ecCurve, int validDays, String^ suffix)
{
	this->path = path;
	this->keySize = keySize;
	this->validDays = validDays;
	this->keyAlg = algorithm;
	this->curveName = ecCurve;
	this->suffix = suffix;
	if (suffix == nullptr)
		this->suffix = "";
	//Init other paths
	this->configPath = Path::Combine(path, "config.conf");
	this->pkiPath = Path::Combine(path, "pki");
	this->caPath = Path::Combine(this->pkiPath, "ca.crt");
	this->keyPath = Path::Combine(this->pkiPath, "ca.key");
	this->crlPath = Path::Combine(this->pkiPath, "crl.crt");
	this->clientsPath = Path::Combine(path, "clients");
}

bool Interactive::LoadConfig()
{
	if (!File::Exists(this->configPath)) {
		return false;
	}

	Dictionary<String^, Object^>^ dict;
	try {
		StreamReader^ sr = gcnew StreamReader(this->configPath);
		String^ json = sr->ReadToEnd();
		sr->Close();

		//Turn into dict
		dict = JsonConvert::DeserializeObject<Dictionary<String^, Object^>^>(json);
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to load config at {0}. {1}", this->configPath, e->Message);
		return false;
	}

	//Load in object
	if ((this->cSubject = CertificateSubject::fromDict(dict)) == nullptr) {
		Console::WriteLine("ERROR: Failed to load subject from config");
		return false;
	}
	this->config = dict;

	//Load in fixed defaults
	Object^ val;
	if (dict->TryGetValue("keysize", val)) {
		this->keySize = Convert::ToInt32(val);		
	}
	else {
		this->keySize = 2048;
	}
	if (dict->TryGetValue("validdays", val)) {
		this->validDays = Convert::ToInt32(val);
	}
	else {
		// Default
		this->validDays = 3650;
	}
	if (dict->TryGetValue("serial", val)) {
		this->_serial = Convert::ToInt32(val);
	}
	else {
		Console::WriteLine("ERROR: Failed to load serial from config");
		return false;
	}
	if (dict->TryGetValue("algorithm", val)) {
		try {
			this->keyAlg = static_cast<OpenSSLHelper::Algorithm>(Convert::ToInt32(val));
		}
		catch (Exception^ _) {
			this->keyAlg = OpenSSLHelper::Algorithm::RSA;
		}
	}
	else {
		this->keyAlg = OpenSSLHelper::Algorithm::RSA;
	}
	this->curveName = nullptr;
	if (dict->TryGetValue("eccurve", val)) {
		this->curveName = (String^)val;
	}
	else if (this->keyAlg == OpenSSLHelper::Algorithm::EdDSA) {
		this->curveName = "ED25519";
	}
	else {
		this->curveName = "secp384r1";
	}

	if (dict->TryGetValue("suffix", val))
		this->suffix = (String^)val;
	else
		this->suffix = "";

	//Load in CA
	String^ certData;
	try {
		StreamReader^ sr = gcnew StreamReader(this->caPath);
		certData = sr->ReadToEnd();
		sr->Close();
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to read cert off disk. " + e->Message);
		return false;
	}
	String^ keyData;
	try {
		StreamReader^ sr = gcnew StreamReader(this->keyPath);
		keyData = sr->ReadToEnd();
		sr->Close();
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to read key off disk. " + e->Message);
		return false;
	}

	try {
		this->Issuer = OpenSSLHelper::LoadIdentity(certData, keyData);
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to load issuer identity. {0}", e->Message);
		return false;
	}
	if (this->Issuer == nullptr) {
		Console::WriteLine("ERROR: Failed to load issuer identity, empty value.");
		return false;
	}

	return true;
}

bool Interactive::SaveConfig()
{
	this->config["serial"] = this->_serial;
	try {
		//Convert to JSON
		String^ json = JsonConvert::SerializeObject(this->config);

		//Write to config
		StreamWriter^ sw = gcnew StreamWriter(this->configPath);
sw->Write(json);
sw->Flush();
sw->Close();
	}
	catch (Exception ^ e) {
		Console::WriteLine("ERROR: Failed to write config to {0}. {1}", this->configPath, e->Message);
		return false;
	}

	return true;
}

bool Interactive::CreateNewIssuer()
{
	if (cSubject == nullptr) {
		Console::WriteLine("ERROR: No Subject available.");
		return false;
	}
	Identity^ identity;
	try {
		identity = OpenSSLHelper::CreateCAAndKey(cSubject, this->keyAlg, this->keySize, this->curveName, this->validDays, this->Serial);
	}
	catch (Exception ^ e) {
		Console::WriteLine("ERROR: Failed to create CA. {0}", e->Message);
		return false;
	}
	this->Issuer = identity;
	return this->saveIdentity(identity, "ca");
}

bool Interactive::CreateDH()
{
	Console::WriteLine("Creating DH Params. This will take a while...");
	try {
		String^ dhPem = OpenSSLHelper::CreateDH(this->keySize);

		//Save to disk
		String^ dhPath = Path::Combine(this->pkiPath, "dh.pem");
		StreamWriter^ sw = gcnew StreamWriter(dhPath);
		sw->Write(dhPem);
		sw->Flush();
		sw->Close();
		Console::WriteLine(); //Write blank line to gap the dots
	}
	catch (Exception ^ e) {
		Console::WriteLine("ERROR: Failed to generate DH params. {0}", e->Message);
		return false;
	}
	return true;
}

bool Interactive::CreateServerConfig()
{
	String^ caName = "ca" + this->suffix + ".crt";
	String^ crlName = "crl" + this->suffix + ".crt";
	String^ certName = "server" + this->suffix + ".crt";
	String^ certpath = Path::Combine(this->pkiPath, "server.crt");
	String^ keyName = "server" + this->suffix + ".key";
	String^ keypath = Path::Combine(this->pkiPath, "server.key");
	String^ dhName = "dh" + this->suffix + ".pem";
	String^ dhPath = Path::Combine(this->pkiPath, "dh.pem");

	if (!File::Exists(this->caPath)) {
		Console::WriteLine("ERROR: Missing CA. Please regenerate config");
		return false;
	}
	if (this->keyAlg == OpenSSLHelper::Algorithm::RSA && !File::Exists(dhPath)) {
		Console::WriteLine("ERROR: Missing DH. Please regenerate config");
		return false;
	}

	if (!File::Exists(certpath) || !File::Exists(keypath)) {
		if (!this->createNewServerIdentity()) {
			Console::WriteLine("ERROR: Failed to generate server identity.");
			return false;
		}
	}

	String^ port;
	String^ proto;
	try {
		port = (String^)this->config["port"];
		proto = (String^)this->config["proto"];
		if (proto == "tcp") {
			proto = "tcp-server";
		}
		else {
			proto = "udp";
		}
	}
	catch (Exception ^ e) {
		Console::WriteLine("ERROR: Invalid config. Please regenerate config. " + e->Message);
		return false;
	}
	if (!File::Exists(certpath)) {
		Console::WriteLine("ERROR: Missing Cert. Please regenerate config");
		return false;
	}
	if (!File::Exists(keypath)) {
		Console::WriteLine("ERROR: Missing Key. Please regenerate config");
		return false;
	}

	String^ file = "#-- Config Auto Generated by SparkLabs OpenVPN Certificate Generator --#\n";
	file += "#--                   Config for OpenVPN 2.4 Server                  --#\n\n";
	file += "proto {0}\n";
	file += "ifconfig-pool-persist ipp" + this->suffix + ".txt\n";
	file += "keepalive 10 120\n";
	file += "user nobody\ngroup nogroup\n";
	file += "persist-key\npersist-tun\n";
	file += "status openvpn-status" + this->suffix + ".log\n";
	file += "verb 3\n";
	file += "mute 10\n";
	file += String::Format("ca {0}\ncert {1}\nkey {2}\n", caName, certName, keyName);
	if (File::Exists(this->crlPath)) {
		file += "crl-verify " + crlName + "\n";
	}
	if (this->keyAlg == OpenSSLHelper::Algorithm::RSA) {
		file += "dh " + dhName + "\n";
	}
	else if (this->keyAlg == OpenSSLHelper::Algorithm::EdDSA) {
		file += "tls-version-min 1.3\n";
		file += "dh none\n";
		file += "# Note this curve probably isn't supported (yet), however OpenVPN will fall back to another (secp384r1)\n";
		file += "ecdh-curve " + this->curveName + "\n";
		file += "tls-cipher TLS_AES_256_GCM_SHA384\n";
	}
	else { // ecdsa
		file += "tls-version-min 1.2\n";
		file += "dh none\n";
		file += "ecdh-curve " + this->curveName + "\n";
		file += "tls-cipher TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384\n";
	}
	file += "port {1}\n";
	file += "dev tun0\n";
	file += "server 10.8.0.0 255.255.255.0\n";

	try {
		List<String^>^ dns = (List<String^>^)config["dns"];
		if (dns != nullptr && dns->Count > 0) {
			for each (String^ var in dns)
			{
				file += String::Format("push \"dhcp-option DNS {0}\"\n", var);
			}
		}
	} catch (Exception^) {}

	try {
		if ((bool)this->config["redirect"]) {
			file += "push \"redirect-gateway def1\"\n";
		}
	} catch (Exception^){}

	file += "#Uncomment the below to allow client to client communication\n#client-to-client\n";
	file += "#Uncomment the below and modify the command to allow access to your internal network\n#push \"route 192.168.0.0 255.255.255.0\"\n";

	file = String::Format(file, proto, port);

	//Make a new directory for the server
	String^ serverPath = Path::Combine(this->path, "server");
	try {
		if (Directory::Exists(serverPath)) {
			Directory::Delete(serverPath, true);
		}
		Directory::CreateDirectory(serverPath);
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to make directory for server configuration. {0}", e->Message);
		return false;
	}

	//Write config
	try {
		StreamWriter^ sw = gcnew StreamWriter(Path::Combine(serverPath, "server" + this->suffix + ".conf"));
		sw->Write(file);
		sw->Flush();
		sw->Close();
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to write server config. {0}", e->Message);
		return false;
	}
	//Copy Files
	try {
		File::Copy(this->caPath, Path::Combine(serverPath, caName), true);
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to copy CA. {0}", e->Message);
		return false;
	}
	try {
		File::Copy(certpath, Path::Combine(serverPath, certName), true);
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to copy Cert. {0}", e->Message);
		return false;
	}
	try {
		if (this->keyAlg == OpenSSLHelper::Algorithm::RSA)
			File::Copy(dhPath, Path::Combine(serverPath, dhName), true);
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to copy DH. {0}", e->Message);
		return false;
	}
	try {
		File::Copy(keypath, Path::Combine(serverPath, keyName), true);
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to copy Key. {0}", e->Message);
		return false;
	}
	try {
		if (File::Exists(this->crlPath)) {
			File::Copy(this->crlPath, Path::Combine(serverPath, crlName), true);
		}
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to copy CRL. {0}", e->Message);
		return false;
	}
	Console::WriteLine("Successfully generated server configuration at {0}.", serverPath);
	return true;
}

bool Interactive::CreateNewClientConfig(String ^ name)
{
	if (cSubject == nullptr) {
		Console::WriteLine("ERROR: No subject available.");
		return false;
	}
	if (!File::Exists(this->caPath)) {
		Console::WriteLine("ERROR: Missing CA. Please regenerate config.");
		return false;
	}

	String^ address;
	String^ port;
	String^ proto;
	try {
		address = (String^)this->config["server"];
		port = (String^)this->config["port"];
		proto = (String^)this->config["proto"];
		if (proto == "tcp") {
			proto = "tcp-client";
		}
		else {
			proto = "udp";
		}
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Invalid config. Please regenerate config. " + e->Message);
		return false;
	}

	//Try and make dir for all clients if not exists
	try {
		if (!Directory::Exists(this->clientsPath)) {
			Directory::CreateDirectory(this->clientsPath);
		}
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to make clients directory. {0}", e->Message);
		return false;
	}

	String^ CN;
	if (!String::IsNullOrWhiteSpace(name)) {
		CN = name;
	}
	else {
		String^ input = askQuestion("Common Name. This should be unique, for example a username [client1]:", false);
		if (String::IsNullOrWhiteSpace(input)) {
			CN = "client1";
		}
		else {
			CN = input;
		}
	}
	String^ clientPath = Path::Combine(this->path, CN);
	try {
		if (Directory::Exists(clientPath)) {
			Directory::Delete(clientPath, true);
		}
		Directory::CreateDirectory(clientPath);
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to make directory for server configuration. {0}", e->Message);
		return false;
	}

	if (!createNewClientIdentity(CN))
		return false;
	
	//Copy files
	String^ cert = String::Format("{0}.crt", CN);
	String^ key = String::Format("{0}.key", CN);
	try {
		File::Copy(this->caPath, Path::Combine(clientPath, "ca.crt"));
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to copy CA. {0}", e->Message);
		return false;
	}
	try {
		File::Copy(Path::Combine(pkiPath, cert), Path::Combine(clientPath, cert));
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to copy Cert. {0}", e->Message);
		return false;
	}
	try {
		File::Copy(Path::Combine(pkiPath, key), Path::Combine(clientPath, key));
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to copy Key. {0}", e->Message);
		return false;
	}

	//Create config
	String^ file = "#-- Config Auto Generated By SparkLabs OpenVPN Certificate Generator--#\n\n";
	file += "#viscosity name {0}@{1}\n";
	file += "remote {1} {2} {3}\n";
	file += "dev tun\ntls-client\n";
	//Certs
	file += "ca ca.crt\n";
	file += "cert {0}.crt\n";
	file += "key {0}.key\n";
	file += "persist-tun\npersist-key\nnobind\npull\n";
	if (this->keyAlg == OpenSSLHelper::Algorithm::EdDSA) {
		file += "tls-version-min 1.3\n";
	}
	else if (this->keyAlg == OpenSSLHelper::Algorithm::ECDSA) {		
		file += "tls-version-min 1.2\n";
		file += "tls-cipher TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384\n";
	}

	file = String::Format(file, CN, address, port, proto);

	//Write config
	try {
		StreamWriter^ sw = gcnew StreamWriter(Path::Combine(clientPath, "config.conf"));
		sw->Write(file);
		sw->Flush();
		sw->Close();
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to write client config. {0}", e->Message);
		return false;
	}

	//Create visc
	this->createVisz(CN, clientPath);

	//remove config
	try {
		if (Directory::Exists(clientPath)) {
			Directory::Delete(clientPath, true);
		}
	}
	catch (Exception^) {
		//Do nothing
	}

	return true;
}

String ^ Interactive::askQuestion(String ^ question, bool allowedBlank, bool hasDefault)
{
	while (true) {
		Console::Write(question + " ");
		String^ input = Console::ReadLine();
		if (String::IsNullOrWhiteSpace(input) && !hasDefault) {
			Console::WriteLine("This field cannot be left blank.");
			continue;
		}
		if (input == "." && !allowedBlank) {
			Console::WriteLine("This field cannot be left blank.");
			continue;
		}
		return input;
	}
}
String ^ Interactive::askQuestion(String ^ question, bool allowedBlank) {
	return askQuestion(question, allowedBlank, true);
}

bool Interactive::saveIdentity(Identity^ identity, String^ name)
{
	//Create PKI dir
	try {
		if (!Directory::Exists(this->pkiPath))
			Directory::CreateDirectory(this->pkiPath);
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to create PKI dir. {0}", e->Message);
	}

	String^ certpath = Path::Combine(this->pkiPath, name + ".crt");
	String^ keypath = Path::Combine(this->pkiPath, name + ".key");

	String^ cert = OpenSSLHelper::CertAsPEM(identity->cert);
	if (cert == nullptr) {
		Console::WriteLine("ERROR: Failed to create certificate");
		return false;
	}
	try {
		StreamWriter^ sw = gcnew StreamWriter(certpath);
		sw->Write(cert);
		sw->Flush();
		sw->Close();
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to write certificate to disk. {0}", e->Message);
	}

	String^ key = OpenSSLHelper::KeyAsPEM(identity->key);
	if (key == nullptr) {
		Console::WriteLine("ERROR: Failed to create key");
		return false;
	}
	try {
		StreamWriter^ sw = gcnew StreamWriter(keypath);
		sw->Write(key);
		sw->Flush();
		sw->Close();
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to write key to disk. {0}", e->Message);
	}

	return true;
}

bool Interactive::createNewClientIdentity(String ^ name)
{
	if (!verifyRequirements())
		return false;
	CertificateSubject^ subject = this->cSubject;
	subject->CommonName = name;
	Identity^ identity;
	try {
		identity = OpenSSLHelper::CreateCertKeyBundle(subject, this->Issuer, this->keyAlg, this->keySize, this->curveName, this->validDays, this->Serial, false);
	}
	catch (Exception^ e) {
		Console::WriteLine("Failed to create server identity. {0}", e->Message);
		return false;
	}
	return saveIdentity(identity, name);
}

bool Interactive::createNewServerIdentity()
{
	if (!verifyRequirements())
		return false;
	CertificateSubject^ subject = this->cSubject;
	subject->CommonName = "server";
	Identity^ identity;
	try {
		identity = OpenSSLHelper::CreateCertKeyBundle(subject, this->Issuer, this->keyAlg, this->keySize, this->curveName, this->validDays, this->Serial, true);
	}
	catch (Exception^ e) {
		Console::WriteLine("Failed to create server identity. {0}", e->Message);
		return false;
	}
	return saveIdentity(identity, "server");
}

bool Interactive::createVisz(String^ fileName, String ^ folder)
{
	String^ visz = Path::Combine(this->clientsPath, String::Format("{0}.visz", fileName));
	Stream^ outStream = File::Create(visz);
	Stream^ gzoStream = gcnew GZipOutputStream(outStream);
	TarArchive^ tarArchive = TarArchive::CreateOutputTarArchive(gzoStream);

	String^ rootPath = Directory::GetParent(folder)->ToString();
	tarArchive->RootPath = rootPath->Replace('\\', '/');
	if (tarArchive->RootPath->EndsWith("/")) {
		tarArchive->RootPath = tarArchive->RootPath->Remove(tarArchive->RootPath->Length);
	}

	//Add files
	String^ currentDir = Directory::GetCurrentDirectory();
	try {
		// We can only have one directory deep in the tar, so set the current directory in case the folder isn't
		// at the current root, then use the filename as it will be the dir name
		Directory::SetCurrentDirectory(rootPath);
		TarEntry^ entry = TarEntry::CreateEntryFromFile(fileName);
		entry->Name = fileName;
		tarArchive->WriteEntry(entry, true);
		tarArchive->Close();
		return true;
	}
	finally {
		Directory::SetCurrentDirectory(currentDir);
	}
}

bool Interactive::verifyRequirements()
{
	Console::WriteLine("Creating Server Identity...");
	if (this->Issuer == nullptr) {
		Console::WriteLine("ERROR: No issuer available.");
		return false;
	}
	if (this->cSubject == nullptr) {
		Console::WriteLine("ERROR: No subject available.");
		return false;
	}
	return true;
}

bool Interactive::GenerateNewConfig()
{
	//First check a config doesn't already exist here
	if (LoadConfig()) {
		Console::WriteLine("ERROR: Config already exists, please choose a different directory");
		return false;
	}
	Console::WriteLine("Please fill in the information below that will be incorporated into your certificate.");
	Console::WriteLine("Some fields have a default value in square brackets, simply press Enter to use these values without entering anything.");
	Console::WriteLine("Some fields can be left blank if desired. Enter a '.' only for a field to be left blank.");
	Console::WriteLine("---");

	if (this->keyAlg == OpenSSLHelper::Algorithm::EdDSA) {
		while (true) {
			Console::WriteLine("IMPORTANT!!!");
			Console::WriteLine("You have selected to use EdDSA. EdDSA support is currently experimental.");
			Console::WriteLine("Please note EdDSA keys and configurations will only work with Viscosity 1.8.2+, and OpenVPN 2.4.7+ & OpenSSL 1.1.1+ on your server.");
			String^ input = askQuestion("Continue? [Y/n]:", false)->ToLower();
			if (input == String::Empty || input == "y") {
				break;
			}
			else if (input == "n") {
				Environment::Exit(0);
			}
			Console::WriteLine("Invalid input, try again.");
		}
	}

	String^ address = askQuestion("Server address, e.g. myserver.mydomain.com:", false, false);

	String^ port;
	while (true) {
		String^ input = askQuestion(String::Format("Server Port [{0}]:", defaultPort), false);
		if (input == String::Empty) {
			port = defaultPort;
			break;
		}
		//Check
		int test;
		if (int::TryParse(input, test) && test > 0 && test < 65535) {
			port = input;
			break;
		}
		else {
			Console::WriteLine("Invalid input, try again.");
		}
	}

	String^ proto;
	while (true) {
		String^ input = askQuestion(String::Format("Protocol, 1=UDP, 2=TCP [{0}]:", defaultProtocol), false);
		if (input == String::Empty) {
			proto = defaultProtocol->ToLower();
			break;
		}
		if (input == "1") {
			proto = "udp";
			break;
		}
		else if (input == "2") {
			proto = "tcp";
			break;
		}
		Console::WriteLine("Invalid input, try again");
	}

	bool redirectTraffic = true;
	while (true) {
		String^ input = askQuestion("Redirect all traffic through VPN? [Y/n]:", false)->ToLower();
		if (input == String::Empty || input == "y") {
			break;
		}
		else if (input == "n") {
			redirectTraffic = false;
			break;
		}
		Console::WriteLine("Invalid input, try again.");
	}

	List<String^>^ dns = gcnew List<String^>();

	int defaultDNSChoice;
	bool customDNS = false;
	if (redirectTraffic)
		defaultDNSChoice = 1;
	else
		defaultDNSChoice = 4;

	Console::WriteLine("Please specify DNS servers to push to connecting clients:");
	Console::WriteLine(String::Format("\t1 - CloudFlare ({0})", String::Join(" & ", cloudflareDNS)));
	Console::WriteLine(String::Format("\t2 - Google ({0})", String::Join(" & ", googleDNS)));
	Console::WriteLine(String::Format("\t3 - OpenDNS ({0})", String::Join(" & ", openDNS)));
	Console::WriteLine(String::Format("\t4 - Local Server ({0}). You will need a DNS server running beside your VPN server", localDNS));
	Console::WriteLine("\t5 - Custom");
	Console::WriteLine("\t6 - None");
	
	while (true) {
		String^ input = askQuestion(String::Format("Please select an option [{0:D}]:", defaultDNSChoice), true);
		if (String::IsNullOrEmpty(input)) {
			if (defaultDNSChoice == 1)
				dns->AddRange(cloudflareDNS);
			else
				dns->Add(localDNS);
		}
		else if (input == "1")
			dns->AddRange(cloudflareDNS);
		else if (input == "2")
			dns->AddRange(googleDNS);
		else if (input == "3")
			dns->AddRange(openDNS);
		else if (input == "4")
			dns->Add(localDNS);
		else if (input == "5")
			customDNS = true;
		else if (input == "6" || input == ".")
			break;
		else {
			Console::WriteLine(String::Format("{0} is not a valid choice", input));
			continue;
		}
		// Default will continue, so we can break here
		break;
	}

	if (customDNS) {
		while (true) {
			String^ input = askQuestion("Enter Custom DNS Servers, comma separated for multiple:", false);
			if (String::IsNullOrWhiteSpace(input))
				continue;
			//Try and split whatever input was given
			dns->Clear();
			array<String^>^ vals = input->Split(gcnew array<String^>{ "," }, StringSplitOptions::RemoveEmptyEntries);
			System::Net::IPAddress^ discard;
			bool valid = true;
			for each (String^ var in vals)
			{
				String^ tmp = var->Trim();
				if (System::Net::IPAddress::TryParse(tmp, discard)) {
					dns->Add(tmp);
				}
				else {
					Console::WriteLine(tmp + " is not a valid IP Address.");
					valid = false;
					break;
				}
			}
			if (valid)
				break;
		}
	}

	bool useDefaults = true;
	while (true) {
		String^ input = askQuestion("Would you like to use anonymous defaults for certificate details? [Y/n]:", false)->ToLower();
		if (input == String::Empty || input == "y") {
			break;
		}
		else if (input == "n") {
			useDefaults = false;
			break;
		}
		Console::WriteLine("Invalid input, try again.");
	}
	CertificateSubject^ cs;
	String^ input;
	if (useDefaults) {
		cs = gcnew CertificateSubject(address);
		goto SAVEDETAILS;
	}

	input = askQuestion(String::Format("Common Name, e.g. your servers name [{0}]:", address), false);
	if (input == String::Empty) {
		input = address;
	}

	cs = gcnew CertificateSubject(input);
		
	input = askQuestion(String::Format("Country Name, 2 letter ISO code [{0}]:", defaultCountry), true);
	if (input == String::Empty) {
		input = defaultCountry;
	}
	if (input != ".") {
		cs->Country = input;
	}

	input = askQuestion(String::Format("State or Province [{0}]:", defaultState), true);
	if (input == String::Empty) {
		input = defaultState;
	}
	if (input != ".") {
		cs->State = input;
	}

	input = askQuestion(String::Format("Locality Name, e.g. a City [{0}]:", defaultLocale), true);
	if (input == String::Empty) {
		input = defaultLocale;
	}
	if (input != ".") {
		cs->Location = input;
	}

	input = askQuestion(String::Format("Organisation Name [{0}]:", defaultON), true);
	if (input == String::Empty) {
		input = defaultON;
	}
	if (input != ".") {
		cs->Organisation = input;
	}

	input = askQuestion(String::Format("Organisation Unit, e.g. department [{0}]:", defaultOU), true);
	if (input == String::Empty) {
		input = defaultOU;
	}
	if (input != ".") {
		cs->OrganisationUnit = input;
	}

	input = askQuestion(String::Format("Email Address [{0}]:", defaultEmail), true);
	if (input == String::Empty) {
		input = defaultEmail;
	}
	if (input != ".") {
		cs->Email = input;
	}

	SAVEDETAILS:

	Dictionary<String^, Object^>^ config = cs->toDict();
	config->Add("proto", proto);
	config->Add("port", port);
	config->Add("server", address);
	config->Add("redirect", redirectTraffic);
	config->Add("keysize", this->keySize);
	config->Add("validdays", this->validDays);
	config->Add("dns", dns);
	config->Add("algorithm", this->keyAlg);
	config->Add("eccurve", this->curveName);
	config->Add("suffix", this->suffix);

	this->config = config;
	this->cSubject = cs;

	return this->SaveConfig();
}

bool Interactive::RevokeCert(String ^ name)
{
	if (!Directory::Exists(this->pkiPath)) {
		Console::WriteLine("ERROR: There are no certificates to revoke.");
		return false;
	}
	String^ CN;
	if (!String::IsNullOrWhiteSpace(name)) {
		CN = name;
	}
	else {
		String^ input = askQuestion("Common Name of certificate to revoke:", false);
		if (String::IsNullOrWhiteSpace(input)) {
			return false;
		}
		else {
			CN = input;
		}
	}
	// Make sure we don't revoke ourself
	if (CN == "cert") {
		Console::WriteLine("ERROR: Cannot revoke this.");
		return false;
	}
	// Find the certificate
	String^ certname = String::Format("{0}.crt", CN);
	String^ certpath = Path::Combine(pkiPath, certname);
	String^ certData;
	if (!File::Exists(certpath)) {
		Console::WriteLine("ERROR: Certificate not found.");
		return false;
	}
	else {
		try {
			StreamReader^ sr = gcnew StreamReader(certpath);
			certData = sr->ReadToEnd();
			sr->Close();
		}
		catch (Exception^ e) {
			Console::WriteLine("ERROR: Failed to read certificate off disk. " + e->Message);
			return false;
		}
	}
	String^ crlData;
	if (File::Exists(this->crlPath)) {
		try {
			StreamReader^ sr = gcnew StreamReader(this->crlPath);
			crlData = sr->ReadToEnd();
			sr->Close();
			Console::WriteLine("Existing CRL found and will be appended to.");
		}
		catch (Exception^ e) {
			Console::WriteLine("ERROR: Failed to read CRL off disk. " + e->Message);
			return false;
		}
	}
	else {
		crlData = nullptr;
		Console::WriteLine("No existing CRL was found, a new CRL will be created.");
	}

	// Create/Update CRL
	try {
		crlData = OpenSSLHelper::CreateCRL(this->Issuer, this->keyAlg, crlData, certData, this->validDays);
	}
	catch (Exception^ e) {
		Console::WriteLine("Failed to create CRL. {0}", e->Message);
		return false;
	}

	// Write the file to disk
	try {
		StreamWriter^ sw = gcnew StreamWriter(this->crlPath);
		sw->Write(crlData);
		sw->Flush();
		sw->Close();
	}
	catch (Exception^ e) {
		Console::WriteLine("ERROR: Failed to write CRL to disk. {0}", e->Message);
		return false;
	}

	// Delete the PKI and configuration for this user
	try {
		File::Delete(certpath);
	}
	catch (Exception^ e) {
		Console::WriteLine(String::Format("WARNING: Failed to remove revoked PKI data. {0}", e->Message));
	}
	String^ keypath = Path::Combine(this->pkiPath, String::Format("{0}.key", CN));
	try {
		File::Delete(keypath);
	}
	catch (Exception^ e) {
		Console::WriteLine(String::Format("WARNING: Failed to remove revoked PKI data. {0}", e->Message));
	}
	String^ confPath = Path::Combine(this->clientsPath, String::Format("{0}.visz", CN));
	try {
		File::Delete(confPath);
	}
	catch (Exception^ e) {
		Console::WriteLine(String::Format("WARNING: Failed to remove revoked PKI data. {0}", e->Message));
	}

	Console::WriteLine();
	Console::WriteLine(String::Format("\"{0}\" has been successfully revoked. The CRL file has been saved to \"{1}\".", CN, this->crlPath));
	Console::WriteLine("Please leave a copy of the CRL file in place if you wish to update it in the future.");
	Console::WriteLine();
	String^ input = askQuestion("Regenerate Server configuration? [Y/n]:", false)->ToLower();
	if (input == String::Empty || input == "y") {
		this->CreateServerConfig();
	}

	return true;
}
