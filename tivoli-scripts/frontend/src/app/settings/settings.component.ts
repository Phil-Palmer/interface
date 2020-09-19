import { Component, OnInit } from "@angular/core";
import { ActivatedRoute } from "@angular/router";

@Component({
	selector: "app-settings",
	templateUrl: "./settings.component.html",
	styleUrls: ["./settings.component.scss"],
})
export class SettingsComponent implements OnInit {
	constructor(private readonly route: ActivatedRoute) {}

	getRouteName() {
		if (this.route.snapshot.firstChild) {
			return this.route.snapshot.firstChild.data.name;
		}
	}
	getRouteIcon() {
		if (this.route.snapshot.firstChild) {
			return this.route.snapshot.firstChild.data.icon;
		}
	}

	ngOnInit() {}
}
